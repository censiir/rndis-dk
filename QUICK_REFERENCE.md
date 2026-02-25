# RNDIS Data Transfer - Quick Reference Card

## Core Data Structure

```cpp
// Ethernet frame size: 60-1500 bytes (typically 64 bytes min)
//
// Wrapped in RNDIS:
// ┌─────────────────────────────────────┐
// │ RNDISPacketMessage (28 bytes)       │
// │  - type = 0x00000001 (PACKET_MSG)   │
// │  - messageLength = 28 + frameSize   │
// │  - dataOffset = 28 (from &type)     │
// │  - dataLength = frameSize           │
// └─────────────────────────────────────┘
// ┌─────────────────────────────────────┐
// │ Ethernet Frame (60-1500 bytes)      │
// │ [Destination MAC (6)]               │
// │ [Source MAC (6)]                    │
// │ [EtherType (2)] → 0x0806=ARP, ...  │
// │ [IP/Data] (variable)                │
// └─────────────────────────────────────┘
```

## Essential Functions

### Wrap Frame for Sending
```cpp
uint32_t wrappedLen;
WrapEthernetFrame(framePtr, frameLen, bufPtr, bufSize, &wrappedLen);
// Result: bufPtr now contains RNDIS wrapper + frame
```

### Unwrap Received Frame
```cpp
uint8_t *frame;
uint32_t frameLen;
UnwrapPacketMessage(rndisMsg, msgLen, &frame, &frameLen);
// Result: frame points to Ethernet data, frameLen is size
```

### Send Frame to Device
```cpp
kern_return_t ret = SendBulkOutData(framePtr, frameLen);
// Takes care of wrapping and USB transfer
```

### Receive Frames from Device
```cpp
// Set up at Start():
StartBulkInListening();

// Receives automatically, calls BulkInAsyncCompletion when data arrives
// Completion handler unwraps and logs data

// At Stop():
StopBulkInListening();
```

## Endpoint Addresses

- **Interrupt IN (0x82):** Status notifications (optional, not used for data)
- **Bulk IN (0x81):** Device → Host, frames received
- **Bulk OUT (0x01):** Host → Device, frames sent

## Integration Checklist

```
ADD TO rndis_dk.cpp (in namespace):
├─ struct RNDISPacketMessage (after other structs)
├─ static constexpr uint32_t kRNDISMsgPacket = 0x00000001U
├─ struct ActiveBulkInRequest
├─ static ActiveBulkInRequest gActiveBulkIn
├─ WrapEthernetFrame()
├─ UnwrapPacketMessage()
├─ SendBulkOutData()
├─ BulkInAsyncCompletion() [with forward declaration]
├─ StartBulkInListening()
├─ StopBulkInListening()
└─ Update Start() and Stop() methods

UPDATE in rndis_dk.cpp:
├─ RNDISDriverState struct: add "bool bulkInListening"
├─ ResetStateValues(): add "gState.bulkInListening = false"
├─ Start(): call StartBulkInListening()
└─ Stop(): call StopBulkInListening()
```

## Code Locations in rndis_dk.cpp

| Code | Insert After | Line (~) |
|------|--------------|----------|
| Constants | kRNDISMsgKeepAliveComplete | 45 |
| Structs | RNDISKeepAliveComplete | 150 |
| Globals | RNDISDriverState | 200 |
| Wrap/Unwrap | AllocateControlBuffer() | 500 |
| SendBulkOut | Wrap/Unwrap functions | 600 |
| Async handlers | SendBulkOut | 750 |
| Replace Start() | InitializeRNDISDevice() | 1160 |
| Replace Stop() | IMPL(Start) | 1190 |

## Test Commands

```bash
# Build
xcodebuild -scheme rndis-dk -configuration Debug

# Load
launchctl load ~/Library/LaunchDaemons/rndis-dk.plist

# Watch logs
log stream --predicate 'process contains "rndis"' --level debug

# Check if initialized
log stream | grep "bulk IN listening started"

# Send test packet from Python
cd /Users/ubayd/rndis-dk
.venv/bin/python scripts/test_usb.py

# Verify reception
log stream | grep -A2 "received.*bytes"
```

## Common Errors & Fixes

| Error | Cause | Fix |
|-------|-------|-----|
| "unknown type 'RNDISPacketMessage'" | Struct not defined | Add struct definition |
| "use of undeclared identifier 'gActiveBulkIn'" | Global not declared | Add `static ActiveBulkInRequest gActiveBulkIn = {}` |
| "failed to start bulk IN listening ret=0xe00002c1" | bulkInPipe is NULL | Verify data interface opened, pipes created |
| "received 0 bytes" repeatedly | Device not sending | Verify with Python test_usb.py, check packet filter |
| "unwrap failed ret=0xe00002d2" | RNDIS packet malformed | Check raw bytes, verify type=0x01000000 |
| No "bulk IN listening started" | StartBulkInListening() failed | Check return code in logs |

## Key Values

```cpp
// Data transfer
constexpr uint32_t kBulkInBufferSize = 64 * 1024;   // Receive buffer
constexpr uint32_t kMaxWrapBufferSize = 16 * 1024;  // Send buffer
constexpr uint32_t kRNDISMsgPacket = 0x00000001U;   // Type
constexpr uint32_t kDataOffset = 28U;               // From Type field
constexpr uint32_t kAlignment = 64U;                // Padding alignment

// Endpoints
const uint8_t kBulkInAddr = 0x81;   // Device → Host
const uint8_t kBulkOutAddr = 0x01;  // Host → Device
```

## Async Transfer Pattern

All bulk IN receives happen asynchronously:

```
1. StartBulkInListening() called at Start()
   ├─ Creates IOBufferMemoryDescriptor (64KB)
   └─ Calls bulkInPipe->AsyncTransfer(..., BulkInAsyncCompletion)

2. Device sends frame → USB completes
   └─ BulkInAsyncCompletion() called with data

3. Handler processes frame
   ├─ Unwraps RNDIS message
   ├─ Logs frame details
   └─ Calls AsyncTransfer again to receive next frame

4. Repeats until Stop() calls StopBulkInListening()
   └─ Calls bulkInPipe->Abort() → kIOReturnAborted
   └─ Handler returns without resubmitting
```

## Memory Management

**Transmit:**
```cpp
// Memory allocated per send
IOBufferMemoryDescriptor::Create(...) → must release with OSSafeReleaseNULL()
```

**Receive:**
```cpp
// Single persistent buffer allocated at Start()
gActiveBulkIn.descriptor created in StartBulkInListening()
Released in StopBulkInListening()
```

## Logging Best Practices

```cpp
// Data received
os_log(OS_LOG_DEFAULT, "rndis-dk: received %u bytes", bytesTransferred);

// Frame unwrapped
os_log(OS_LOG_DEFAULT, "rndis-dk: extracted frame length=%u", frameLength);

// Send result
os_log(OS_LOG_DEFAULT, "rndis-dk: sent %u byte frame (wrapped to %u)",
       frameLength, wrappedLength);

// Errors
os_log(OS_LOG_DEFAULT, "rndis-dk: error ret=0x%x", ret);

// Check logs:
log stream | grep "rndis-dk:"
```

## Verification Steps

```
✓ Step 1: Initialization
  └─ See: "initialized control-if=0 data-if=1..."

✓ Step 2: Listening Started
  └─ See: "bulk IN listening started"

✓ Step 3: Data Received
  └─ See: "received X bytes on bulk IN"

✓ Step 4: Frame Unwrapped
  └─ See: "extracted Ethernet frame length=X"

✓ Step 5: Send Works
  └─ Call SendBulkOutData(frame, len)
  └─ See: "sent X byte frame"

✓ Step 6: No Crashes or Leaks
  └─ Run for 5+ minutes
  └─ Memory usage stable
  └─ No crashes on Stop
```

## One-Liner Tests

```bash
# Is device present?
ioreg -l -p IOService | grep 6864

# Is extension loaded?
launchctl list | grep rndis

# Recent RNDIS logs?
log stream --predicate 'message contains "rndis"' --level debug | head -20

# Memory leak check (simple)?
ps aux | grep rndis

# Network interface visible? (after IONetworkController)
ifconfig | grep -i rndis

# Test device with Python?
cd /Users/ubayd/rndis-dk && .venv/bin/python scripts/test_usb.py
```

## File References

| File | Purpose |
|------|---------|
| `rndis_dk.cpp` | Main implementation (modify this) |
| `rndis_dk.iig` | DriverKit interface (no changes) |
| `rndis_data_transfer.cpp` | Code snippets (copy from) |
| `RNDIS_IMPLEMENTATION_GUIDE.md` | Deep dive explanation |
| `STEP_BY_STEP_INTEGRATION.md` | Line-by-line walkthrough |
| `DEBUGGING_REFERENCE.md` | Troubleshooting guide |
| `test_usb.py` | Python test utility |

## Key Insights

- 🎯 **RNDIS wraps Ethernet frames** in 28-byte header before bulk transfer
- 🔄 **Bulk IN is asynchronous** - continuously polls for frames
- 📤 **Bulk OUT is synchronous** - sends on demand
- 💾 **Allocate once (IN), allocate per-transfer (OUT)** for memory efficiency
- 🔍 **Every frame is logged** - makes debugging easy
- ✋ **Callbacks are short** - resubmit immediately after processing
- 🛑 **Abort on stop** - prevents resubmit after Close()

## Remember

1. **Test early, test often** - Don't wait to test complete integration
2. **Check logs constantly** - They tell you exactly what's happening
3. **Compare with Python** - Your test_usb.py is the reference
4. **One change at a time** - Integration is easier than debugging
5. **Memory leaks are easy to spot** - Watch RSS in `ps aux`
6. **Async callbacks are tricky** - Keep them simple, no locks
7. **Documentation is your friend** - Re-read when stuck

---

**Good luck! You've got all the code and guidance you need.** 🚀
