# RNDIS Debugging Reference

## Expected Log Sequence

### Successful Full Initialization

```
rndis-dk: initialized control-if=0 data-if=1 bulkIn=0x81 bulkOut=0x01 maxTransfer=16384
rndis-dk: bulk IN listening started
```

This means:
- ✓ USB interfaces discovered correctly
- ✓ RNDIS control negotiation succeeded
- ✓ Bulk endpoints identified
- ✓ Async listener active

### When Receiving Data

Once device sends Ethernet frames:

```
rndis-dk: received 42 bytes on bulk IN
rndis-dk: extracted Ethernet frame length=28
rndis-dk: dest MAC ff:ff:ff:ff:ff:ff
rndis-dk: EtherType 0x0806
```

This means:
- ✓ Data arriving on bulk IN endpoint
- ✓ RNDIS unwrapping working
- ✓ Frame extracted properly

---

## Common Error Patterns & Solutions

### Error 1: Missing "bulk IN listening started"

```
rndis-dk: initialized control-if=0 data-if=1 bulkIn=0x81 bulkOut=0x01 maxTransfer=16384
(NO: rndis-dk: bulk IN listening started)
```

**Cause:** `StartBulkInListening()` failed

**Debug steps:**
```bash
# Check what error code was returned:
log stream --predicate 'message contains "failed to start bulk IN"'

# Likely errors:
# ret=0xe00002c1 - Invalid argument (bulkInPipe is NULL)
# ret=0xe00002c0 - Not found (pipe creation failed)
```

**Solutions:**
1. Verify bulk IN endpoint was found: `bulkInEndpointAddress` should be `0x81`
2. Check if `gState.bulkInPipe` is being opened correctly in `OpenInterfacesAndPipes()`
3. Verify data interface is being opened correctly

Sample fix check in code:
```cpp
os_log(OS_LOG_DEFAULT, "rndis-dk: DEBUG bulkInPipe=%p bulkInAddr=0x%x",
       gState.bulkInPipe, gState.bulkInEndpointAddress);
```

### Error 2: "received 0 bytes"

```
rndis-dk: received 0 bytes on bulk IN
rndis-dk: received 0 bytes on bulk IN
rndis-dk: received 0 bytes on bulk IN
```

**Cause:** Transfer completing with 0 bytes (no data)

**Debug steps:**

1. Check packet filter was set: Should see in Python logs "Packet filter response: ..." with valid hex
2. Verify device is actually sending data: Use Python to force it:

```python
# In test_usb.py, after packet filter set:
time.sleep(0.5)
ret = SendTestARPRequest()  # Use function from test_usb.py
time.sleep(1)
# Check logs for data arrival
```

**Solutions:**
1. Device might not be in a state to send data
   - Try resetting device: `sudo killall -9 rndis-dext && wait 1 && reboot`
2. Packet filter setting might have failed
   - Add logging before and after: `os_log(..., "BEFORE SetOID");` and `os_log(..., "AFTER SetOID");`
3. Check bulk IN endpoint address is correct
   - Should be `0x81` (IN direction)
   - Verify with: `log stream | grep "bulkIn=0x"`

### Error 3: "unwrap failed ret=0xe00002d2"

```
rndis-dk: received 64 bytes on bulk IN
rndis-dk: unwrap failed ret=0xe00002d2 (kIOReturnUnderrun)
```

**Cause:** RNDIS packet is incomplete or malformed

**Debug steps:**

1. Examine raw bytes received:

Add this to `BulkInAsyncCompletion`:
```cpp
os_log(OS_LOG_DEFAULT, "rndis-dk: raw[0:8]= %02x %02x %02x %02x %02x %02x %02x %02x",
       gActiveBulkIn.buffer[0], gActiveBulkIn.buffer[1], gActiveBulkIn.buffer[2],
       gActiveBulkIn.buffer[3], gActiveBulkIn.buffer[4], gActiveBulkIn.buffer[5],
       gActiveBulkIn.buffer[6], gActiveBulkIn.buffer[7]);
```

2. Decode the type field (bytes 0-3, little-endian):
   - `01 00 00 00` = 0x00000001 = PACKET_MSG ✓ (correct)
   - `07 00 00 00` = 0x00000007 = INDICATE_STATUS (OID notification, should skip)
   - Anything else = problem

3. Check message length (bytes 4-7):
   - Should be ≤ `bytesTransferred`
   - Should be reasonable (8 bytes min header, 60+ bytes typical Ethernet)

**Solutions:**

1. If seeing INDICATE_STATUS (0x07), that's OK - code skips it but should unwrap after
2. If message length > transfer size, might be incomplete receive
   - Increase buffer size in `StartBulkInListening()`: Currently 64KB, can go larger
3. If seeing garbage, might be firmware issue
   - Test with Python to ensure device works

### Error 4: "bulk out transfer failed ret=0xe00002d1"

When trying to send data via `SendBulkOutData()`:

```
rndis-dk: bulk out transfer failed ret=0xe00002d1(kIOReturnNoResources)
```

**Cause:** Memory descriptor creation failed

**Debug steps:**
```cpp
// Add to SendBulkOutData before return:
os_log(OS_LOG_DEFAULT, "rndis-dk: wrap frame %u -> %u bytes",
       frameLength, rndisMessageLength);

// Check descriptor creation:
os_log(OS_LOG_DEFAULT, "rndis-dk: buffer address=%p", wrapBuffer);
```

**Solutions:**
1. Increase memory available to extension
2. Try smaller test frames initially
3. Check if previous transfers are being cleaned up properly

---

## Manual Testing Workflow

### Test 1: Verify Initialization Only

```bash
# Just load extension, don't send data
launchctl load ~/Library/LaunchDaemons/rndis-dk.plist

# Check for success log
log stream --level debug | grep rndis-dk | grep "initialized"

# Should see:
# rndis-dk: initialized control-if=0 data-if=1 bulkIn=0x81 bulkOut=0x01 maxTransfer=16384
```

**Success criteria:** Sees "initialized" log
**Troubleshoot if:** Doesn't appear or shows different interface numbers

### Test 2: Verify Bulk IN Listening Started

```bash
# Same extension should log:
log stream --level debug | grep rndis-dk | grep "listening"

# Should see:
# rndis-dk: bulk IN listening started
```

**Success criteria:** "listening started" appears in logs within 1 second of load
**Troubleshoot if:** Doesn't appear → `StartBulkInListening()` failed

### Test 3: Verify Data Reception (Manual)

With extension loaded:

```bash
# In terminal 1: Watch logs
log stream --predicate 'process contains "rndis"' --level debug

# In terminal 2: Send test ARP from Python
cd /Users/ubayd/rndis-dk && .venv/bin/python3 -c "
import usb1
import struct
import time

ctx = usb1.USBContext()
handle = ctx.openByVendorIDAndProductID(0x04E8, 0x6864)

if handle:
    # Wrap simple ARP in RNDIS_PACKET_MSG
    arp = bytes.fromhex('ffffffffffff000000000001080600010800060400010000000000010c0a80164000000000000c0a80101')
    rndis = struct.pack('<IIIIII', 0x00000001, 28+len(arp), 0, 0, 0, 0) + arp
    
    # Send via bulk OUT (endpoint 0x01)
    endp = handle.getDevice().iterConfigurations()[0][1].iterSettings()[0].iterEndpoints()
    # [simplified - would need proper endpoint discovery]
    
    print(f'Sent {len(rndis)} bytes')
    time.sleep(1)
"
```

**Expected log output:**
```
rndis-dk: received XX bytes on bulk IN
rndis-dk: extracted Ethernet frame length=42
rndis-dk: dest MAC ff:ff:ff:ff:ff:ff
rndis-dk: EtherType 0x0806
```

### Test 4: Loopback Test (Extension echoing back)

Modify `BulkInAsyncCompletion` to echo frames:

```cpp
// In BulkInAsyncCompletion, after successfully unwrapping:
if (ret == kIOReturnSuccess && frameLength > 0U)
{
    // Echo the frame back (for testing)
    os_log(OS_LOG_DEFAULT, "rndis-dk: echoing frame back to host");
    kern_return_t echoRet = SendBulkOutData(ethernetFrame, frameLength);
    os_log(OS_LOG_DEFAULT, "rndis-dk: echo result ret=0x%x", echoRet);
}
```

Then test with Python:
```python
# Send frame
# Should immediately see it echo back
# Log: rndis-dk: echoing frame back
# Log: rndis-dk: received XX bytes (the echo)
```

---

## USB Tracing Tools

### Option 1: System Profiler (GUI)

```bash
# Open System Profiler
open /Applications/Utilities/System\ Information.app

# Go to: USB
# Select dext device
# Note endpoint addresses and transfer directions
```

### Option 2: USB Prober (Command line)

```bash
# Install (if not present)
brew install usb-prober

# Run
usbprober -d 04e8:6864

# Shows device tree with all endpoints
```

### Option 3: IOKit Registry (Low-level)

```bash
# List all USB devices and their properties
ioreg -l -p IOService | grep -A 20 "rndis"

# Look for:
# - "idProduct" = 0x6864
# - "idVendor" = 0x04E8
# - Interface numbers
```

### Option 4: Instruments (Real-time)

1. Open Xcode
2. Xcode → Open Developer Tool → Instruments
3. Select "System Trace"
4. Add track: USB Device
5. Record 10 seconds
6. View filtered events

Shows every USB transfer in real-time with timing.

---

## Memory & Performance Debugging

### Check Extension Memory Usage

```bash
# While extension is loaded and receiving data
ps aux | grep rndis

# Look at RSS column (resident set size)
# Should be < 10 MB

# If growing unbounded, memory leak possible
# Check for missing OSSafeReleaseNULL() calls
```

### Profile with Instruments

1. Build with symbols: `xcodebuild -scheme rndis-dk -configuration Debug`
2. Load extension
3. Open Instruments → Choose "Leaks"
4. Target process: kernel/extension
5. Record while sending/receiving data
6. Look for red "leaks" in graph

---

## Log Level Verbosity

### Current Logging (Info Level)

Default setup logs key operations.

### To Add Trace-Level Logging (For Deep Debug)

Replace `os_log` calls with conditional logging:

```cpp
#define DEBUG_RNDIS 1

#if DEBUG_RNDIS
    #define LOG_DEBUG(fmt, ...) os_log(OS_LOG_DEFAULT, fmt, ##__VA_ARGS__)
#else
    #define LOG_DEBUG(fmt, ...) do {} while(0)
#endif

// Usage:
LOG_DEBUG("rndis-dk: trace event %u", value);
```

Then controls verbosity at compile time.

### Using Runtime Debug Flags

```cpp
// In RNDISDriverState
bool debugVerbose;

// In Start():
gState.debugVerbose = (getenv("RNDIS_DEBUG") != nullptr);

// Usage:
if (gState.debugVerbose) {
    os_log(OS_LOG_DEFAULT, "rndis-dk: [DEBUG] %u bytes", bytesTransferred);
}
```

Then enable with:
```bash
RNDIS_DEBUG=1 launchctl load ...
```

---

## Validation Checklist

Before claiming "data transfer works":

- [ ] Initialization log shows correct interface numbers
- [ ] Bulk IN listening started
- [ ] Device sends ARP → Extension logs "received X bytes"
- [ ] Frame unwraps successfully
- [ ] Destination MAC logged correctly (first 6 bytes)
- [ ] Multiple receive cycles work (not just first one)
- [ ] Can send via `SendBulkOutData()` without errors (watch logs)
- [ ] Device receives sent data (visible in Wireshark if real network)
- [ ] No memory leaks over 5+ minutes continuous operation
- [ ] Unload/reload doesn't crash extension

If all pass → Data transfer is solid, ready for network stack integration!
