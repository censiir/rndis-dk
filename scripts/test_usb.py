import usb1
import struct
import time

VENDOR_ID = 0x04E8
PRODUCT_ID = 0x6864

# RNDIS message types
SEND_ENCAPSULATED_COMMAND = 0x00
GET_ENCAPSULATED_RESPONSE = 0x01

REMOTE_NDIS_QUERY_MSG = 0x00000003
REMOTE_NDIS_SET_MSG   = 0x00000005

# RNDIS OIDs
OID_GEN_CURRENT_PACKET_FILTER   = 0x0001010E
OID_GEN_MAXIMUM_FRAME_SIZE      = 0x00010106

# Packet filter bits
FILTER_DIRECTED  = 0x00000001
FILTER_BROADCAST = 0x00000002
FILTER_MULTICAST = 0x00000004
FILTER_ALL       = FILTER_DIRECTED | FILTER_BROADCAST | FILTER_MULTICAST

def send_control(handle, request, data):
    return handle.controlWrite(
        0x21,  # Host-to-device class
        request,
        0,
        0,
        data
    )

def get_response(handle, length=1024):
    try:
        return handle.controlRead(
            0xA1,  # Device-to-host class
            GET_ENCAPSULATED_RESPONSE,
            0,
            0,
            length
        )
    except Exception as e:
        print("GET_ENCAPSULATED_RESPONSE error:", e)
        return None

def rndis_query_msg(oid, request_id=2):
    """
    Build a RNDIS Query message for the given OID
    """
    payload = struct.pack("<I", oid)
    msglen = 20 + len(payload)
    return struct.pack(
        "<IIIIII",
        REMOTE_NDIS_QUERY_MSG,
        msglen,
        request_id,
        oid,
        len(payload),
        0  # reserved
    ) + payload

def rndis_set_packet_filter(filter_bits, request_id=3):
    """
    Build a RNDIS Set message to enable packet receipt
    """
    payload = struct.pack("<I", OID_GEN_CURRENT_PACKET_FILTER) + struct.pack("<I", filter_bits)
    msglen = 20 + len(payload)
    return struct.pack(
        "<IIIIII",
        REMOTE_NDIS_SET_MSG,
        msglen,
        request_id,
        OID_GEN_CURRENT_PACKET_FILTER,
        len(payload) - 4,  # size of value only
        0
    ) + payload

with usb1.USBContext() as ctx:
    dev = ctx.openByVendorIDAndProductID(VENDOR_ID, PRODUCT_ID)
    if not dev:
        print("Device not found")
        exit(1)

    # find and open the RNDIS control interface
    handle = None
    for cfg in dev.getDevice().iterConfigurations():
        for intf in cfg:
            for setting in intf.iterSettings():
                if setting.getClass() == 0xE0 and setting.getSubClass() == 0x01 and setting.getProtocol() == 0x03:
                    print("Found control interface", setting.getNumber())
                    dev.claimInterface(setting.getNumber())
                    handle = dev
                    break
            if handle:
                break
        if handle:
            break

    if not handle:
        print("RNDIS control interface not found")
        exit(1)

    # Step 1: Query maximum frame size
    query_msg = rndis_query_msg(OID_GEN_MAXIMUM_FRAME_SIZE, request_id=2)
    print("Sending OID_GEN_MAXIMUM_FRAME_SIZE query")
    send_control(handle, SEND_ENCAPSULATED_COMMAND, query_msg)
    time.sleep(0.1)
    resp = get_response(handle)
    print("Response:", None if resp is None else resp.hex())

    # Step 2: Set packet filter
    print("Sending packet filter SET")
    set_msg = rndis_set_packet_filter(FILTER_ALL, request_id=3)
    send_control(handle, SEND_ENCAPSULATED_COMMAND, set_msg)
    time.sleep(0.1)
    resp = get_response(handle)
    print("Packet filter response:", None if resp is None else resp.hex())

    print("Initialization complete — device should now start sending/receiving packets")