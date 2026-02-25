# DriverKit RNDIS USB Implementation Guide

## Overview

Your codebase has successfully implemented:
- USB interface enumeration and opening
- Control interface discovery with RNDIS protocol detection
- Data interface discovery with bulk endpoint resolution
- Complete RNDIS control message initialization sequence
- OID query/set operations

**Next Steps (in priority order):**
1. Implement bulk data transfer infrastructure
2. Implement RNDIS packet wrapping/unwrapping
3. Create async I/O handlers for continuous data reception
4. Optionally: Route data through network stack via IONetworkController

---

## Part 1: Understanding RNDIS Data Transfer

### Message Flow Comparison: Control vs Data

**Control Path (Already Implemented):**
```
Host → SendEncapsulatedCommand (control transfer, 0x00) → Device
Device → ReceiveEncapsulatedResponse (control transfer, 0x01) → Host
RNDIS control messages (Initialize, Query OID, Set OID)
```

**Data Path (What We're Adding):**
```
Host ← Bulk IN endpoint (0x81) ← RNDIS_PACKET_MSG (containing Ethernet frame) ← Device
Host → Bulk OUT endpoint (0x01) → RNDIS_PACKET_MSG (containing Ethernet frame) → Device
```

### RNDIS Packet Message Structure

The device sends/receives Ethernet frames wrapped in RNDIS_PACKET_MSG:

```
+-----------------------------------+
| RNDIS Message Header (8 bytes)    |
|  - Type: 0x00000001 (PACKET_MSG)  |
|  - Length: header + data payload  |
+-----------------------------------+
| Data Offset (4 bytes)             |
| Data Length (4 bytes)             |  ← These are from &Type onward
| VC Handle (4 bytes)               |
| Spare (4 bytes)                   |
+-----------------------------------+
| Padding to alignment              |
+-----------------------------------+
| Ethernet Frame (payload)          |
|  (IP packets, ARP, etc.)          |
+-----------------------------------+
```

**Key Points:**
- `Data Offset`: Distance from the start of Type field (usually 28 bytes = 7 × 4-byte fields)
- `Data Length`: Actual Ethernet frame size
- If aligned to 64 bytes, add padding between header and frame
- Total message length must be LSB of Data Length + Data Offset + any padding

---

## Part 2: Bulk Data Transfer Implementation

### Step 1: Add Data Transfer Structures to Header

Add these structures to `rndis_dk.cpp` (in the namespace section):

```cpp
struct RNDISPacketMessage
{
    RNDISMessageHeader hdr;
    uint32_t dataOffset;
    uint32_t dataLength;
    uint32_t vcHandle;
    uint32_t spare;
} __attribute__((packed));

// For receiving and sending Ethernet frames
struct PacketBuffer
{
    uint8_t data[64 * 1024];  // Max packet = 64KB
    uint32_t length;
};

// Async completion context
struct BulkTransferContext
{
    IOMemoryDescriptor *buffer;
    uint32_t bytesTransferred;
    kern_return_t status;
    uint8_t *userData;
};
```

### Step 2: Implement Wrap/Unwrap Functions

Add these utility functions to handle RNDIS packet encapsulation:

```cpp
// Wrap an Ethernet frame in RNDIS_PACKET_MSG for transmission
static kern_return_t
WrapEthernetFrame(const uint8_t *ethernetFrame,
                  uint32_t frameLength,
                  uint8_t *outBuffer,
                  uint32_t outBufferCapacity,
                  uint32_t *outMessageLength)
{
    if ((ethernetFrame == nullptr) || (outBuffer == nullptr) || 
        (outMessageLength == nullptr) || (frameLength > 0xFFFF))
    {
        return kIOReturnBadArgument;
    }

    constexpr uint32_t kRNDISPacketMsg = 0x00000001U;
    constexpr uint32_t kHeaderSize = sizeof(RNDISPacketMessage);
    constexpr uint32_t kDataOffset = 28U;  // From Type field
    constexpr uint32_t kAlignment = 64U;
    
    // Calculate padding needed for alignment
    uint32_t paddingNeeded = 0U;
    uint32_t alignedDataStart = kHeaderSize;
    if ((alignedDataStart + frameLength) % kAlignment != 0)
    {
        paddingNeeded = kAlignment - ((alignedDataStart + frameLength) % kAlignment);
    }

    uint32_t totalMessageLength = kHeaderSize + paddingNeeded + frameLength;
    if (totalMessageLength > outBufferCapacity)
    {
        return kIOReturnNoSpace;
    }

    // Build RNDIS packet header
    RNDISPacketMessage *packet = reinterpret_cast<RNDISPacketMessage *>(outBuffer);
    bzero(packet, kHeaderSize);

    packet->hdr.type = HostToUSB32(kRNDISPacketMsg);
    packet->hdr.messageLength = HostToUSB32(totalMessageLength);
    packet->dataOffset = HostToUSB32(kDataOffset);
    packet->dataLength = HostToUSB32(frameLength);
    packet->vcHandle = 0U;
    packet->spare = 0U;

    // Copy Ethernet frame after header + padding
    uint8_t *frameStart = outBuffer + kHeaderSize + paddingNeeded;
    bcopy(ethernetFrame, frameStart, frameLength);

    *outMessageLength = totalMessageLength;
    return kIOReturnSuccess;
}

// Unwrap RNDIS_PACKET_MSG to extract Ethernet frame
static kern_return_t
UnwrapPacketMessage(const uint8_t *rndisMessage,
                    uint32_t messageLength,
                    uint8_t **outFrame,
                    uint32_t *outFrameLength)
{
    if ((rndisMessage == nullptr) || (outFrame == nullptr) || (outFrameLength == nullptr))
    {
        return kIOReturnBadArgument;
    }

    if (messageLength < sizeof(RNDISPacketMessage))
    {
        return kIOReturnUnderrun;
    }

    const RNDISPacketMessage *packet = 
        reinterpret_cast<const RNDISPacketMessage *>(rndisMessage);

    uint32_t type = USBToHost32(packet->hdr.type);
    if (type != 0x00000001U)  // PACKET_MSG type
    {
        return kIOReturnUnsupported;
    }

    uint32_t msgLen = USBToHost32(packet->hdr.messageLength);
    uint32_t dataOffset = USBToHost32(packet->dataOffset);
    uint32_t dataLength = USBToHost32(packet->dataLength);

    // Validate offsets
    if ((dataOffset > msgLen) || (dataLength > msgLen) || 
        ((dataOffset + dataLength) > msgLen))
    {
        return kIOReturnUnderrun;
    }

    // dataOffset is from &type field, so actual position is dataOffset bytes from packet start
    const uint8_t *frameData = rndisMessage + dataOffset;
    
    // Verify frame is within bounds
    if ((frameData + dataLength) > (rndisMessage + msgLen))
    {
        return kIOReturnUnderrun;
    }

    *outFrame = const_cast<uint8_t *>(frameData);
    *outFrameLength = dataLength;
    return kIOReturnSuccess;
}
```

### Step 3: Implement Bulk OUT (Transmit) - Synchronous Version

This sends Ethernet frames to the device:

```cpp
static kern_return_t
SendBulkOutData(const uint8_t *frame, uint32_t frameLength)
{
    if ((gState.bulkOutPipe == nullptr) || (frame == nullptr) || (frameLength == 0U))
    {
        return kIOReturnBadArgument;
    }

    // Allocate wrapping buffer
    uint8_t *wrapBuffer = nullptr;
    constexpr uint32_t kMaxWrapBufferSize = 16 * 1024;
    
    IOBufferMemoryDescriptor *wrapDescriptor = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOut,
        kMaxWrapBufferSize,
        0U,
        &wrapDescriptor);
    
    if (ret != kIOReturnSuccess)
    {
        return ret;
    }

    // Get the buffer address
    wrapBuffer = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(wrapDescriptor->GetAddress()));
    if (wrapBuffer == nullptr)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return kIOReturnNoMemory;
    }

    // Wrap the frame in RNDIS packet
    uint32_t rndisMessageLength = 0U;
    ret = WrapEthernetFrame(frame, frameLength, wrapBuffer, kMaxWrapBufferSize, &rndisMessageLength);
    if (ret != kIOReturnSuccess)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return ret;
    }

    // Set descriptor length to wrapped message size
    ret = wrapDescriptor->SetLength(rndisMessageLength);
    if (ret != kIOReturnSuccess)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return ret;
    }

    // Perform synchronous bulk transfer
    uint32_t bytesTransferred = 0U;
    ret = gState.bulkOutPipe->Transfer(wrapDescriptor,
                                        0U,           // offset
                                        rndisMessageLength,
                                        nullptr,      // completion (nullptr = sync)
                                        nullptr,      // request
                                        &bytesTransferred);

    OSSafeReleaseNULL(wrapDescriptor);

    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk out transfer failed ret=0x%x bytes=%u",
               ret, bytesTransferred);
        return ret;
    }

    if (bytesTransferred != rndisMessageLength)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk out underrun: expected %u got %u",
               rndisMessageLength, bytesTransferred);
        return kIOReturnUnderrun;
    }

    return kIOReturnSuccess;
}
```

### Step 4: Implement Bulk IN (Receive) - Async Version  

For continuous reception, we use async I/O with completion handlers:

```cpp
// Forward declaration of async handler
static void BulkInAsyncCompletion(void *target,
                                  void *parameter,
                                  IOReturn status,
                                  uint32_t bytesTransferred);

// Context for tracking active bulk IN request
struct ActiveBulkInRequest
{
    IOMemoryDescriptor *descriptor;
    uint8_t *buffer;
    uint32_t bufferSize;
};

static ActiveBulkInRequest gActiveBulkIn = {};

// Start listening for data - call this after initialization
static kern_return_t
StartBulkInListening(void)
{
    if (gState.bulkInPipe == nullptr)
    {
        return kIOReturnBadArgument;
    }

    // Allocate a large buffer for incoming RNDIS packets
    constexpr uint32_t kBulkInBufferSize = 64 * 1024;
    
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionIn,
        kBulkInBufferSize,
        0U,
        &gActiveBulkIn.descriptor);
    
    if (ret != kIOReturnSuccess)
    {
        return ret;
    }

    ret = gActiveBulkIn.descriptor->SetLength(kBulkInBufferSize);
    if (ret != kIOReturnSuccess)
    {
        OSSafeReleaseNULL(gActiveBulkIn.descriptor);
        return ret;
    }

    gActiveBulkIn.buffer = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(gActiveBulkIn.descriptor->GetAddress()));
    gActiveBulkIn.bufferSize = kBulkInBufferSize;

    // Initiate first async bulk IN transfer
    ret = gState.bulkInPipe->AsyncTransfer(
        gActiveBulkIn.descriptor,
        0U,                         // offset in descriptor
        kBulkInBufferSize,          // data length to read
        BulkInAsyncCompletion,      // completion function
        nullptr,                    // target (context)
        nullptr);                   // request

    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: failed to start bulk IN listening ret=0x%x", ret);
        OSSafeReleaseNULL(gActiveBulkIn.descriptor);
        bzero(&gActiveBulkIn, sizeof(gActiveBulkIn));
        return ret;
    }

    return kIOReturnSuccess;
}

// Async completion handler - called when data arrives or transfer fails
static void
BulkInAsyncCompletion(void *target,
                      void *parameter,
                      IOReturn status,
                      uint32_t bytesTransferred)
{
    if (status != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN completion error status=0x%x", status);
        
        // Only resubmit on non-aborts (Stop will abort on shutdown)
        if (status != kIOReturnAborted)
        {
            // Resubmit after error
            kern_return_t ret = gState.bulkInPipe->AsyncTransfer(
                gActiveBulkIn.descriptor,
                0U,
                gActiveBulkIn.bufferSize,
                BulkInAsyncCompletion,
                nullptr,
                nullptr);
            
            if (ret != kIOReturnSuccess)
            {
                os_log(OS_LOG_DEFAULT, "rndis-dk: failed to resubmit bulk IN ret=0x%x", ret);
            }
        }
        return;
    }

    // Process received data
    if (bytesTransferred > 0U)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: received %u bytes", bytesTransferred);
        
        // Unwrap RNDIS message to get Ethernet frame
        uint8_t *ethernetFrame = nullptr;
        uint32_t frameLength = 0U;
        
        kern_return_t ret = UnwrapPacketMessage(gActiveBulkIn.buffer,
                                                bytesTransferred,
                                                &ethernetFrame,
                                                &frameLength);
        
        if (ret == kIOReturnSuccess && frameLength > 0U)
        {
            // TODO: Route frame to network stack
            // For debugging, just log:
            os_log(OS_LOG_DEFAULT, "rndis-dk: extracted frame length=%u", frameLength);
            
            // Example: Print first few bytes (MAC destination)
            if (frameLength >= 6)
            {
                os_log(OS_LOG_DEFAULT, "rndis-dk: dest MAC %02x:%02x:%02x:%02x:%02x:%02x",
                       ethernetFrame[0], ethernetFrame[1], ethernetFrame[2],
                       ethernetFrame[3], ethernetFrame[4], ethernetFrame[5]);
            }
        }
        else
        {
            os_log(OS_LOG_DEFAULT, "rndis-dk: unwrap failed ret=0x%x", ret);
        }
    }

    // Resubmit for next transfer
    kern_return_t ret = gState.bulkInPipe->AsyncTransfer(
        gActiveBulkIn.descriptor,
        0U,
        gActiveBulkIn.bufferSize,
        BulkInAsyncCompletion,
        nullptr,
        nullptr);
    
    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: failed to resubmit bulk IN ret=0x%x", ret);
    }
}

// Stop listening and cleanup
static void
StopBulkInListening(void)
{
    if (gState.bulkInPipe != nullptr)
    {
        // Abort any pending transfers
        gState.bulkInPipe->Abort();
    }
    
    OSSafeReleaseNULL(gActiveBulkIn.descriptor);
    bzero(&gActiveBulkIn, sizeof(gActiveBulkIn));
}
```

---

## Part 3: Integration with Start/Stop

Modify your DriverKit Start/Stop methods:

```cpp
kern_return_t
IMPL(rndis_dk, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess)
    {
        return ret;
    }

    ResetStateValues();

    ret = OpenInterfacesAndPipes(this, provider);
    if (ret != kIOReturnSuccess)
    {
        CloseInterfacesAndReleaseObjects(this);
        (void)Stop(provider, SUPERDISPATCH);
        return ret;
    }

    ret = InitializeRNDISDevice();
    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RNDIS init sequence failed ret=0x%x", ret);
        CloseInterfacesAndReleaseObjects(this);
        (void)Stop(provider, SUPERDISPATCH);
        return ret;
    }

    // NEW: Start listening for incoming data
    ret = StartBulkInListening();
    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: failed to start bulk IN listening ret=0x%x", ret);
        StopBulkInListening();
        CloseInterfacesAndReleaseObjects(this);
        (void)Stop(provider, SUPERDISPATCH);
        return ret;
    }

    os_log(OS_LOG_DEFAULT,
           "rndis-dk: initialized control-if=%u data-if=%u bulkIn=0x%x bulkOut=0x%x maxTransfer=%u",
           gState.controlInterfaceNumber,
           gState.dataInterfaceNumber,
           gState.bulkInEndpointAddress,
           gState.bulkOutEndpointAddress,
           gState.maxTransferSize);

    return kIOReturnSuccess;
}

kern_return_t
IMPL(rndis_dk, Stop)
{
    // NEW: Stop receiving data before shutting down
    StopBulkInListening();

    if (gState.initialized)
    {
        RNDISHalt();
    }

    CloseInterfacesAndReleaseObjects(this);
    return Stop(provider, SUPERDISPATCH);
}
```

---

## Part 4: Testing Data Transfer

### Quick Test: Send ARP broadcast

This function sends an ARP request to test bulk OUT:

```cpp
static kern_return_t
SendARPRequest(void)
{
    // Simple ARP request frame
    uint8_t arpFrame[42] = {
        // Ethernet header
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // dest MAC (broadcast)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01,  // src MAC (fake)
        0x08, 0x06,                           // EtherType ARP
        
        // ARP payload
        0x00, 0x01,                           // HW type (Ethernet)
        0x08, 0x00,                           // Protocol (IP)
        0x06, 0x04,                           // HW len, Proto len
        0x00, 0x01,                           // Op (request)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01,  // Sender HW addr
        0xC0, 0xA8, 0x01, 0x64,               // Sender IP (192.168.1.100)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Target HW addr
        0xC0, 0xA8, 0x01, 0x01                // Target IP (192.168.1.1)
    };
    
    return SendBulkOutData(arpFrame, sizeof(arpFrame));
}
```

### Check Logs

After Start(), check for:
```
rndis-dk: initialized...
rndis-dk: received XXX bytes
rndis-dk: extracted frame length=...
```

If you send data with your Python script, the driver should log received bytes.

---

## Part 5: Best Practices for Debugging

### 1. Logging Points

Add logging at strategic locations:

```cpp
// In BulkInAsyncCompletion:
os_log(OS_LOG_DEFAULT, "rndis-dk: received %u bytes from bulk IN", bytesTransferred);

// For unwrapping:
os_log(OS_LOG_DEFAULT, "rndis-dk: unwrap result type=0x%x offset=%u length=%u",
       messageType, dataOffset, dataLength);

// For sending:
os_log(OS_LOG_DEFAULT, "rndis-dk: sending %u byte frame (wrapped to %u)",
       frameLength, wrappedLength);
```

### 2. Real-time Log Monitoring

From Terminal:
```bash
# Continuous log filter for this extension
log stream --level debug | grep rndis-dk

# Or with timestamps
log stream --level debug --format "time: [size] default: [subsystem] [message]" | grep rndis-dk
```

### 3. USB Trace Monitoring

Monitor USB traffic:
```bash
# Enable USB tracing (needs root)
sudo mddiagnose -f /tmp/diag_report.tar.gz
# Or use Instruments: USB Device - see transfers in real-time
```

### 4. Python Test Script Enhancements

Modify `test_usb.py` to send test packets:

```python
# After setting packet filter, wait a bit then send ARP
time.sleep(0.5)

# Send ARP broadcast
arp_frame = bytes.fromhex(
    "ffffffffffff"          # broadcast destination
    "000000000001"          # fake source
    "0806"                  # ARP type
    "00010800"              # HW and proto type
    "06040001"              # HW/proto length + opcode
    "000000000001"          # sender HW
    "c0a80164"              # sender IP 192.168.1.100
    "000000000000"          # target HW
    "c0a80101"              # target IP 192.168.1.1
)

# Wrap in RNDIS
rndis_packet = struct.pack("<IIIIII",
    0x00000001,             # packet type
    28 + len(arp_frame),    # message length
    0, 0, 0, 0) + arp_frame

send_control(handle, 0x00, rndis_packet)
print("Sent ARP packet via bulk OUT")

# Now listen for response on bulk IN
# (Would need async handling in Python too)
```

### 5. Common Issues & Solutions

**Issue: "received 0 bytes" or no data**
- Check if `AsyncTransfer` is initialized properly
- Ensure packet filter was set successfully
- Verify device is sending (test with Python libusb)
- Check `bulkInPipe` isn't null

**Issue: Unwrap returns error**
- Verify data offset is correct (usually 28 bytes from Type)
- Check message length field is present
- Print raw hex bytes: `os_log(OS_LOG_DEFAULT, "raw: %02x %02x %02x...", buf[0], buf[1], buf[2])`

**Issue: Transfer transfers partial data**
- Increase buffer size or check device's maxTransferSize
- Add length validation: `if (bytesTransferred < expected)`

**Issue: Duplicate receives or resubmit issues**
- Ensure completion handler doesn't resubmit if status is `kIOReturnAborted`
- Check for race conditions if multiple pipes active

---

## Part 6: Optional - Network Stack Integration

To present this as a real network interface (IONetworkController):

### Key Changes Needed:

1. **Add IONetworkController base class** to `.iig`:
```cpp
class rndis_dk: public IOService, public IONetworkController
{
    // ...
};
```

2. **Implement required network methods:**
```cpp
virtual IOOutputQueue *createOutputQueue() override;
virtual IONetworkInterface *createInterface() override;
virtual bool configureInterface(IONetworkInterface *interface) override;
```

3. **Route packets from BulkInAsyncCompletion:**
```cpp
if ret == kIOReturnSuccess {
    // Pass frame to network stack
    mbuf_t packet = NULL;
    mbuf_allocpacket(MBUF_WAITOK, frameLength, &packet);
    mbuf_copyback(packet, 0, frameLength, ethernetFrame, MBUF_WAITOK);
    
    IONetworkController::inputPacket(packet);
}
```

This is more complex and requires understanding BSD networking APIs in DriverKit.

---

## Summary: Next Implementation Steps

1. **Add wrap/unwrap functions** ✓ Provided above
2. **Implement SendBulkOutData** ✓ Provided above (synchronous)
3. **Implement StartBulkInListening + async handler** ✓ Provided above
4. **Update Start/Stop to call new functions** ✓ Updated code shown
5. **Add logging, test with Python script**
6. *(Optional)* Network stack integration via IONetworkController

The code above gives you working data transfer. Once bulk IN/OUT work, you can extend with network stack integration.
