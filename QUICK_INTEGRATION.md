# Quick Integration Guide

## How to Add Data Transfer to Your Existing Code

### Step 1: Add the Data Transfer Code

Copy the entire contents of `rndis_data_transfer.cpp` into the namespace section of your `rndis_dk.cpp`, **right after** the existing helper functions (after `InitializeRNDISDevice()` and before `IMPL(rndis_dk, Start)`).

### Step 2: Add to RNDISDriverState struct

In the `RNDISDriverState` struct definition (around line 59), add if not already present:

```cpp
struct RNDISDriverState
{
    // ... existing members ...
    
    bool bulkInListening;  // NEW: track if we're listening for data
};
```

### Step 3: Update ResetStateValues()

In the `ResetStateValues()` function, add:

```cpp
static void
ResetStateValues(void)
{
    // ... existing resets ...
    
    gState.bulkInListening = false;  // NEW
}
```

### Step 4: Update Start() Method

Replace your existing `Start` method with:

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

### Step 5: Update Stop() Method

Replace your existing `Stop` method with:

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

### Step 6: Optional - Test on Start

To test data transfer immediately after initialization, add this to the end of `Start()` (before `return kIOReturnSuccess`):

```cpp
    // Optional: Send test ARP on startup
    // Comment out after debugging
    time_t delay = 0;
    delay += 1;  // 1 second delay
    
    kern_return_t sendRet = SendTestARPRequest();
    os_log(OS_LOG_DEFAULT, "rndis-dk: test ARP send result ret=0x%x", sendRet);
```

(You'll need `#include <sys/time.h>` for proper async delay, or just skip this for now)

---

## File Structure After Integration

```
rndis-dk/
├── rndis_dk.cpp                    (main implementation)
│   ├── Includes
│   ├── Constants (kRNDISMsg*, kOID*, etc.)
│   ├── Structures (RNDIS message types)
│   ├── RNDISDriverState
│   ├── Helper functions
│   │   ├── ResetStateValues()
│   │   ├── FindBulkEndpoints()
│   │   ├── ParseControlInterface()
│   │   └── ... (existing helpers)
│   ├── Control transfer functions
│   │   ├── SendEncapsulatedCommand()
│   │   ├── ReceiveEncapsulatedResponse()
│   │   └── DoRNDISControlTransaction()
│   ├── RNDIS control messages
│   │   ├── RNDISInitialize()
│   │   ├── RNDISQueryOID()
│   │   ├── RNDISSetOID()
│   │   ├── RNDISKeepAlive()
│   │   └── RNDISHalt()
│   ├── **DATA TRANSFER** (add from rndis_data_transfer.cpp)
│   │   ├── WrapEthernetFrame()
│   │   ├── UnwrapPacketMessage()
│   │   ├── SendBulkOutData()
│   │   ├── BulkInAsyncCompletion()
│   │   ├── StartBulkInListening()
│   │   ├── StopBulkInListening()
│   │   └── SendTestARPRequest()
│   ├── InitializeRNDISDevice()
│   ├── IMPL(rndis_dk, Start)     (UPDATED)
│   └── IMPL(rndis_dk, Stop)      (UPDATED)
├── rndis_dk.iig                    (unchanged)
├── rndis-dk.entitlements           (unchanged)
└── Info.plist                      (unchanged)

scripts/
├── test_usb.py                     (existing - can use to send test data)
└── ...

RNDIS_IMPLEMENTATION_GUIDE.md       (detailed explanation)
rndis_data_transfer.cpp             (code snippets to integrate)
QUICK_INTEGRATION.md                (this file)
```

---

## Testing After Integration

### 1. Build the extension

```bash
cd /Users/ubayd/rndis-dk
xcodebuild -scheme rndis-dk -configuration Debug
```

### 2. Check for compilation errors

Look for errors related to:
- Undefined functions (make sure you pasted in the right location)
- Type errors (RNDISPacketMessage struct must be defined)
- Missing includes

### 3. Load the extension

```bash
# Stop if already loaded
sudo killall -9 rndis-dext

# Build fresh
xcodebuild -scheme rndis-dk -configuration Debug

# Load
launchctl load ~/Library/LaunchDaemons/rndis-dk.plist
```

### 4. Check logs in real-time

```bash
log stream --level debug | grep rndis-dk

# You should see:
# rndis-dk: initialized control-if=0 data-if=1 bulkIn=0x81 bulkOut=0x01 maxTransfer=16384
# rndis-dk: bulk IN listening started
```

### 5. Send test data from Python

```bash
cd /Users/ubayd/rndis-dk
.venv/bin/python scripts/test_usb.py

# Then from another terminal:
log stream --level debug | grep rndis-dk

# You should see:
# rndis-dk: received X bytes on bulk IN
# rndis-dk: extracted Ethernet frame length=...
# rndis-dk: dest MAC ...
```

### 6. Send test packets with Python script

Modify `scripts/test_usb.py` to add after packet filter is set:

```python
import time

# ... existing code ...

    print("Packet filter response:", None if resp is None else resp.hex())

    # NEW: Send test packet
    time.sleep(1)  # Wait a bit
    
    # Create RNDIS packet wrapper for a simple ARP broadcast
    arp_data = bytes.fromhex(
        "ffffffffffff"              # broadcast destination
        "000000000001"              # src MAC
        "0806"                      # ARP type
        "00010800"                  # HW/proto
        "06040001"                  # lengths/opcode
        "000000000001"              # src HW
        "c0a80164"                  # src IP 192.168.1.100
        "000000000000"              # target HW
        "c0a80101"                  # target IP 192.168.1.1
    )
    
    # Wrap in RNDIS
    packet_msg = struct.pack("<IIIIII",
        0x00000001,                 # PACKET_MSG type
        28 + len(arp_data),         # message length
        0, 0, 0, 0) + arp_data      # rest of header
    
    # Send via bulk OUT (endpoint 0x01)
    ret = handle.bulkWrite(0x01, packet_msg)
    print(f"Bulk OUT sent: {len(packet_msg)} bytes, ret={ret}")
    
    # Give driver time to process and log
    time.sleep(1)

    print("Test complete")
```

Run it:
```bash
.venv/bin/python scripts/test_usb.py
log stream | grep rndis-dk
```

---

## Troubleshooting Integration

### Issue: Compiler error "unknown type 'RNDISPacketMessage'"

**Solution:** Make sure you're adding `RNDISPacketMessage` struct definition. Check if it's already in your code around line 150. If not, add it to the structures section:

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

### Issue: "error: use of undeclared identifier 'gActiveBulkIn'"

**Solution:** Make sure you're adding the global variable declaration:

```cpp
static ActiveBulkInRequest gActiveBulkIn = {};
```

### Issue: No logs appear, extension hangs

**Solution:** Check if `AsyncTransfer` callback is deadlocking. Common causes:
- Callback trying to acquire locks that are held elsewhere
- Infinite loop in completion handler

Simplest fix: Add early return in `BulkInAsyncCompletion`:

```cpp
static void
BulkInAsyncCompletion(void *target, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    // Safety check first
    if (gState.bulkInPipe == nullptr)
    {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN pipe is NULL, aborting");
        return;
    }
    
    // ... rest of handler ...
}
```

### Issue: Received data is garbage / unwrap fails

**Solution:** Check the raw bytes being received. Add logging:

```cpp
// In BulkInAsyncCompletion, before UnwrapPacketMessage:
os_log(OS_LOG_DEFAULT, "rndis-dk: raw rx: %02x %02x %02x %02x %02x %02x %02x %02x",
       gActiveBulkIn.buffer[0], gActiveBulkIn.buffer[1],
       gActiveBulkIn.buffer[2], gActiveBulkIn.buffer[3],
       gActiveBulkIn.buffer[4], gActiveBulkIn.buffer[5],
       gActiveBulkIn.buffer[6], gActiveBulkIn.buffer[7]);
```

Expected pattern for RNDIS_PACKET_MSG:
- Bytes 0-3: `01 00 00 00` (type in little-endian)
- Bytes 4-7: Message length in little-endian
- etc.

---

## What's Next After Data Transfer Works?

### Phase 1: Verified ✓
- [x] RNDIS control initialization
- [x] Bulk IN/OUT basic transfer

### Phase 2: Next Steps
- [ ] Add proper packet filtering
- [ ] Handle device interrupts (if needed)
- [ ] Implement keep-alive timer

### Phase 3: Network Integration
- [ ] Wrap in IONetworkController
- [ ] Create virtual network interface
- [ ] Route packets to network stack

---

## Questions/Issues?

If integration fails, check:

1. **Compilation**: All functions pasted in correct location (namespace scope, after InitializeRNDISDevice)
2. **Struct definition**: RNDISPacketMessage must be defined before use
3. **Global state**: gActiveBulkIn is declared globally at file level
4. **Method updates**: Start() and Stop() updated as shown above
5. **Logs**: Run `log stream | grep rndis-dk` to see what's happening
