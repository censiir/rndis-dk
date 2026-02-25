# Complete Step-by-Step Integration Example

This document shows exactly where each piece of code goes in your existing `rndis_dk.cpp` file.

## Current File Structure

Your `rndis_dk.cpp` has this layout:

```cpp
// Lines 1-30: Includes and namespace openers
#include ...
namespace { ... }

// Lines 30-100: Constants (kRNDIS*, kCDC*, etc)
static constexpr uint8_t kCDCRequestSendEncapsulatedCommand = ...
// ... more constants ...

// Lines 100-300: Message structures
struct RNDISMessageHeader { ... }
struct RNDISInitializeRequest { ... }
// ... more structures ...

// Lines 300-700: Helper functions
static void ResetStateValues() { ... }
static kern_return_t OpenInterfacesAndPipes() { ... }
static void CloseInterfacesAndReleaseObjects() { ... }

// Lines 700-900: Control transfer functions
static kern_return_t SendEncapsulatedCommand() { ... }
static kern_return_t ReceiveEncapsulatedResponse() { ... }
static kern_return_t DoRNDISControlTransaction() { ... }

// Lines 900-1100: RNDIS message functions
static kern_return_t RNDISInitialize() { ... }
static kern_return_t RNDISQueryOID() { ... }
static kern_return_t RNDISSetOID() { ... }
static kern_return_t RNDISKeepAlive() { ... }
static void RNDISHalt() { ... }

// Lines 1100-1150: Initialization
static kern_return_t InitializeRNDISDevice() { ... }

// Lines 1150-1217: DriverKit entry points
kern_return_t IMPL(rndis_dk, Start) { ... }
kern_return_t IMPL(rndis_dk, Stop) { ... }
// Closing namespace
} // namespace
```

---

## Integration: Data Transfer Code

### 1. Check for RNDISPacketMessage Structure

**Location:** Around line 100-200, in the structure definitions section

**Check if present:**
```cpp
struct RNDISPacketMessage
{
    RNDISMessageHeader hdr;
    uint32_t dataOffset;
    uint32_t dataLength;
    uint32_t vcHandle;
    uint32_t spare;
} __attribute__((packed));
```

**If NOT present, add it** right after other message structures like `RNDISQueryComplete`.

### 2. Add RNDIS_PACKET_MSG Constant

**Location:** Around line 35-50, in the constants section, after other `kRNDISMsg*` constants

**Add:**
```cpp
static constexpr uint32_t kRNDISMsgPacket = 0x00000001U;
```

### 3. Add Data Transfer Structures and Globals

**Location:** Immediately after the `RNDISPacketMessage` structure definition (around line 200)

**Add:**
```cpp
// Context for tracking active bulk IN request
struct ActiveBulkInRequest
{
    IOMemoryDescriptor *descriptor;
    uint8_t *buffer;
    uint32_t bufferSize;
};

static ActiveBulkInRequest gActiveBulkIn = {};
```

### 4. Update RNDISDriverState

**Location:** In the `RNDISDriverState` struct (around line 59-80)

**Add this member:**
```cpp
struct RNDISDriverState
{
    // ... existing members ...
    
    bool bulkInListening;  // NEW: track if we're listening for data
};
```

### 5. Update ResetStateValues()

**Location:** Find the `ResetStateValues()` function (around line 180)

**Add this line at the end:**
```cpp
static void
ResetStateValues(void)
{
    // ... existing code ...
    gState.bulkInListening = false;  // ADD THIS LINE
}
```

### 6. Add Wrap/Unwrap Functions

**Location:** Right after `AllocateControlBuffer()` (around line 500)

**Add these two functions:**

```cpp
// ============================================================================
// RNDIS Message Wrapping/Unwrapping
// ============================================================================

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

    constexpr uint32_t kHeaderSize = sizeof(RNDISPacketMessage);
    constexpr uint32_t kDataOffset = 28U;  // From Type field: 7 uint32_t fields
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

    packet->hdr.type = HostToUSB32(kRNDISMsgPacket);
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
    if (type != kRNDISMsgPacket)
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

    // dataOffset is from &type field
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

### 7. Add Bulk OUT Function

**Location:** Right after the wrap/unwrap functions (around line 600)

```cpp
// ============================================================================
// Bulk OUT (Transmit) - Synchronous
// ============================================================================

static kern_return_t
SendBulkOutData(const uint8_t *frame, uint32_t frameLength)
{
    if ((gState.bulkOutPipe == nullptr) || (frame == nullptr) || (frameLength == 0U))
    {
        return kIOReturnBadArgument;
    }

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

    uint8_t *wrapBuffer = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(wrapDescriptor->GetAddress()));
    if (wrapBuffer == nullptr)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return kIOReturnNoMemory;
    }

    uint32_t rndisMessageLength = 0U;
    ret = WrapEthernetFrame(frame, frameLength, wrapBuffer, kMaxWrapBufferSize, &rndisMessageLength);
    if (ret != kIOReturnSuccess)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return ret;
    }

    ret = wrapDescriptor->SetLength(rndisMessageLength);
    if (ret != kIOReturnSuccess)
    {
        OSSafeReleaseNULL(wrapDescriptor);
        return ret;
    }

    uint32_t bytesTransferred = 0U;
    ret = gState.bulkOutPipe->Transfer(wrapDescriptor,
                                        0U,
                                        rndisMessageLength,
                                        nullptr,
                                        nullptr,
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

    os_log(OS_LOG_DEFAULT, "rndis-dk: sent %u byte frame (wrapped to %u)",
           frameLength, rndisMessageLength);
    return kIOReturnSuccess;
}
```

### 8. Add Bulk IN Async Handler and Related Functions

**Location:** Right after SendBulkOutData() (around line 750)

```cpp
// ============================================================================
// Bulk IN (Receive) - Asynchronous with continuous polling
// ============================================================================

// Forward declaration
static void BulkInAsyncCompletion(void *target,
                                  void *parameter,
                                  IOReturn status,
                                  uint32_t bytesTransferred);

// Async completion handler
static void
BulkInAsyncCompletion(void *target,
                      void *parameter,
                      IOReturn status,
                      uint32_t bytesTransferred)
{
    if (status != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN completion error status=0x%x", status);
        
        if (status != kIOReturnAborted)
        {
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

    if (bytesTransferred > 0U && bytesTransferred <= gActiveBulkIn.bufferSize)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: received %u bytes on bulk IN", bytesTransferred);
        
        uint8_t *ethernetFrame = nullptr;
        uint32_t frameLength = 0U;
        
        kern_return_t ret = UnwrapPacketMessage(gActiveBulkIn.buffer,
                                                bytesTransferred,
                                                &ethernetFrame,
                                                &frameLength);
        
        if (ret == kIOReturnSuccess && frameLength > 0U)
        {
            os_log(OS_LOG_DEFAULT, "rndis-dk: extracted Ethernet frame length=%u", frameLength);
            
            if (frameLength >= 6)
            {
                os_log(OS_LOG_DEFAULT, "rndis-dk: dest MAC %02x:%02x:%02x:%02x:%02x:%02x",
                       ethernetFrame[0], ethernetFrame[1], ethernetFrame[2],
                       ethernetFrame[3], ethernetFrame[4], ethernetFrame[5]);
            }
            
            if (frameLength >= 14)
            {
                uint16_t etherType = (ethernetFrame[12] << 8) | ethernetFrame[13];
                os_log(OS_LOG_DEFAULT, "rndis-dk: EtherType 0x%04x", etherType);
            }
        }
        else if (ret != kIOReturnSuccess)
        {
            os_log(OS_LOG_DEFAULT, "rndis-dk: unwrap failed ret=0x%x", ret);
        }
    }

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

// Start listening for incoming data
static kern_return_t
StartBulkInListening(void)
{
    if (gState.bulkInPipe == nullptr)
    {
        return kIOReturnBadArgument;
    }

    constexpr uint32_t kBulkInBufferSize = 64 * 1024;
    
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionIn,
        kBulkInBufferSize,
        0U,
        &gActiveBulkIn.descriptor);
    
    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: failed to create bulk IN buffer ret=0x%x", ret);
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

    if (gActiveBulkIn.buffer == nullptr)
    {
        OSSafeReleaseNULL(gActiveBulkIn.descriptor);
        return kIOReturnNoMemory;
    }

    ret = gState.bulkInPipe->AsyncTransfer(
        gActiveBulkIn.descriptor,
        0U,
        kBulkInBufferSize,
        BulkInAsyncCompletion,
        nullptr,
        nullptr);

    if (ret != kIOReturnSuccess)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: failed to start bulk IN listening ret=0x%x", ret);
        OSSafeReleaseNULL(gActiveBulkIn.descriptor);
        bzero(&gActiveBulkIn, sizeof(gActiveBulkIn));
        return ret;
    }

    os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN listening started");
    return kIOReturnSuccess;
}

// Stop listening and cleanup
static void
StopBulkInListening(void)
{
    if (gState.bulkInPipe != nullptr)
    {
        gState.bulkInPipe->Abort();
    }
    
    OSSafeReleaseNULL(gActiveBulkIn.descriptor);
    bzero(&gActiveBulkIn, sizeof(gActiveBulkIn));
    os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN listening stopped");
}
```

### 9. Update Start() Method

**Location:** Replace the entire `IMPL(rndis_dk, Start)` function (around line 1160)

**Replace with:**
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
    
    gState.bulkInListening = true;  // NEW

    os_log(OS_LOG_DEFAULT,
           "rndis-dk: initialized control-if=%u data-if=%u bulkIn=0x%x bulkOut=0x%x maxTransfer=%u",
           gState.controlInterfaceNumber,
           gState.dataInterfaceNumber,
           gState.bulkInEndpointAddress,
           gState.bulkOutEndpointAddress,
           gState.maxTransferSize);

    return kIOReturnSuccess;
}
```

### 10. Update Stop() Method

**Location:** Replace the entire `IMPL(rndis_dk, Stop)` function (around line 1190)

**Replace with:**
```cpp
kern_return_t
IMPL(rndis_dk, Stop)
{
    // NEW: Stop receiving data before shutting down
    if (gState.bulkInListening)
    {
        StopBulkInListening();
        gState.bulkInListening = false;
    }

    if (gState.initialized)
    {
        RNDISHalt();
    }

    CloseInterfacesAndReleaseObjects(this);
    return Stop(provider, SUPERDISPATCH);
}
```

---

## Line Number Summary

| Function | Approx Lines | Action |
|----------|--------------|--------|
| Add kRNDISMsgPacket constant | 40-50 | Add 1 line |
| Add RNDISPacketMessage struct | 100-200 | Add 8 lines if missing |
| Add ActiveBulkInRequest struct | ~200 | Add 8 lines |
| Add gActiveBulkIn global | ~200 | Add 1 line |
| Update RNDISDriverState | ~60-80 | Add 1 member |
| Update ResetStateValues() | ~180 | Add 1 line |
| Add WrapEthernetFrame() | ~500 | Add 40 lines |
| Add UnwrapPacketMessage() | ~540 | Add 45 lines |
| Add SendBulkOutData() | ~585 | Add 70 lines |
| Add BulkInAsyncCompletion() | ~655 | Add 80 lines |
| Add StartBulkInListening() | ~735 | Add 70 lines |
| Add StopBulkInListening() | ~805 | Add 10 lines |
| Replace Start() | ~1160 | Replace entirely |
| Replace Stop() | ~1190 | Replace entirely |

**Total additions:** ~400-450 lines of code

---

## Compilation Verification

After making changes, compile to check for errors:

```bash
cd /Users/ubayd/rndis-dk
xcodebuild -scheme rndis-dk -configuration Debug 2>&1 | head -50
```

**Expected:** No errors, only warnings (if any)

**Common compilation issues:**

1. **"unknown type 'RNDISPacketMessage'"**
   - Solution: Verify struct is defined around line 100-200

2. **"use of undeclared identifier 'gActiveBulkIn'"**
   - Solution: Verify global is declared around line 200

3. **"use of undeclared identifier 'BulkInAsyncCompletion'"**
   - Solution: Verify forward declaration is present before use

4. **"no member named 'bulkInListening'"**
   - Solution: Verify RNDISDriverState struct was updated

---

## Testing After Integration

```bash
# 1. Build
xcodebuild -scheme rndis-dk -configuration Debug

# 2. Unload old version if running
launchctl unload ~/Library/LaunchDaemons/rndis-dk.plist 2>/dev/null

# 3. Build and load
xcodebuild -scheme rndis-dk -configuration Debug && \
  launchctl load ~/Library/LaunchDaemons/rndis-dk.plist

# 4. Check logs
log stream --predicate 'process contains "rndis"' --level debug

# Should see:
# rndis-dk: initialized control-if=0 data-if=1 ...
# rndis-dk: bulk IN listening started
```

**Success!** Your extension now has data transfer capability.
