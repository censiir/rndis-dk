//
//  rndis_dk.cpp
//  rndis-dk
//
//  Created by Ubayd on 2/21/26.
//

#include <os/log.h>
#include <stdint.h>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODataQueueDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSObject.h>

#include <USBDriverKit/AppleUSBDefinitions.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>

#include <NetworkingDriverKit/NetworkingDriverKit.h>

#include <net/ethernet.h>

#include "rndis_dk.h"

#undef super
#define super IOUserNetworkEthernet

namespace {

static constexpr uint8_t kRNDISControlClass = 0xE0;
static constexpr uint8_t kRNDISControlSubClass = 0x01;
static constexpr uint8_t kRNDISControlProtocol = 0x03;
static constexpr uint8_t kRNDISDataClass = 0x0A;

static constexpr uint8_t kCDCRequestSendEncapsulatedCommand = 0x00;
static constexpr uint8_t kCDCRequestGetEncapsulatedResponse = 0x01;

static constexpr uint32_t kRNDISMsgInitialize = 0x00000002U;
static constexpr uint32_t kRNDISMsgHalt = 0x00000003U;
static constexpr uint32_t kRNDISMsgQuery = 0x00000004U;
static constexpr uint32_t kRNDISMsgSet = 0x00000005U;
static constexpr uint32_t kRNDISMsgKeepAlive = 0x00000008U;
static constexpr uint32_t kRNDISMsgPacket = 0x00000001U;

static constexpr uint32_t kRNDISMsgInitializeComplete = 0x80000002U;
static constexpr uint32_t kRNDISMsgQueryComplete = 0x80000004U;
static constexpr uint32_t kRNDISMsgSetComplete = 0x80000005U;
static constexpr uint32_t kRNDISMsgKeepAliveComplete = 0x80000008U;
static constexpr uint32_t kRNDISMsgIndicateStatus = 0x00000007U;

static constexpr uint32_t kRNDISStatusSuccess = 0x00000000U;

static constexpr uint32_t kOID8023PermanentAddress = 0x01010101U;
static constexpr uint32_t kOID8023CurrentAddress = 0x01010102U;
static constexpr uint32_t kOIDGenCurrentPacketFilter = 0x0001010EU;

static constexpr uint32_t kPacketFilterDirected = 0x00000001U;
static constexpr uint32_t kPacketFilterAllMulticast = 0x00000004U;
static constexpr uint32_t kPacketFilterBroadcast = 0x00000008U;
static constexpr uint32_t kPacketFilterPromiscuous = 0x00000020U;

static constexpr uint32_t kControlBufferCapacity = 4096U;
static constexpr uint32_t kRNDISPacketHeaderLength = 44U;
static constexpr uint32_t kEthernetMinFrameLength = 60U;
static constexpr uint32_t kBulkInBufferSize = 64 * 1024U;
static constexpr uint32_t kMaxWrapBufferSize = 16 * 1024U;
static constexpr uint32_t kRNDISPollCount = 16U;
static constexpr uint32_t kRNDISTimeoutMs = 3000U;
static constexpr bool kVerboseDataPathLogs = false;

struct RNDISMessageHeader {
    uint32_t type;
    uint32_t messageLength;
} __attribute__((packed));

struct RNDISInitializeRequest {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t majorVersion;
    uint32_t minorVersion;
    uint32_t maxTransferSize;
} __attribute__((packed));

struct RNDISInitializeComplete {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t status;
    uint32_t majorVersion;
    uint32_t minorVersion;
    uint32_t deviceFlags;
    uint32_t medium;
    uint32_t maxPacketsPerTransfer;
    uint32_t maxTransferSize;
    uint32_t packetAlignmentFactor;
    uint32_t afListOffset;
    uint32_t afListSize;
} __attribute__((packed));

struct RNDISQueryRequest {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t oid;
    uint32_t infoLength;
    uint32_t infoOffset;
    uint32_t deviceVcHandle;
} __attribute__((packed));

struct RNDISQueryComplete {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t status;
    uint32_t infoLength;
    uint32_t infoOffset;
} __attribute__((packed));

struct RNDISSetRequest {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t oid;
    uint32_t infoLength;
    uint32_t infoOffset;
    uint32_t deviceVcHandle;
} __attribute__((packed));

struct RNDISSetComplete {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t status;
} __attribute__((packed));

struct RNDISKeepAliveRequest {
    RNDISMessageHeader header;
    uint32_t requestId;
} __attribute__((packed));

struct RNDISKeepAliveComplete {
    RNDISMessageHeader header;
    uint32_t requestId;
    uint32_t status;
} __attribute__((packed));

struct RNDISPacketMessage {
    RNDISMessageHeader header;
    uint32_t dataOffset;
    uint32_t dataLength;
    uint32_t vcHandle;
    uint32_t spare;
    uint32_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
    uint32_t reserved4;
    uint32_t reserved5;
} __attribute__((packed));

struct RNDISDriverState {
    rndis_dk *driver;
    IOUSBHostDevice *device;
    IOUSBHostInterface *controlInterface;
    IOUSBHostInterface *dataInterface;
    IOUSBHostPipe *bulkInPipe;
    IOUSBHostPipe *bulkOutPipe;

    IOBufferMemoryDescriptor *controlBuffer;
    uint8_t *controlBufferBytes;

    IOBufferMemoryDescriptor *bulkInBuffer;
    uint8_t *bulkInBytes;

    OSAction *bulkInAction;
    OSAction *txPacketAction;

    IODispatchQueue *dispatchQueue;
    IOUserNetworkPacketBufferPool *pool;
    IOUserNetworkTxSubmissionQueue *txsQueue;
    IOUserNetworkTxCompletionQueue *txcQueue;
    IOUserNetworkRxSubmissionQueue *rxsQueue;
    IOUserNetworkRxCompletionQueue *rxcQueue;

    uint8_t controlInterfaceNumber;
    uint8_t dataInterfaceNumber;
    uint8_t dataAltSetting;
    uint8_t bulkInEndpointAddress;
    uint8_t bulkOutEndpointAddress;

    uint32_t maxTransferSize;
    uint32_t mtu;
    uint32_t requestId;
    uint32_t packetFilter;

    bool initialized;
    bool bulkInListening;
    bool interfaceEnabled;
    bool promiscuous;
    bool allMulticast;

    IOUserNetworkMACAddress macAddress;
    IOUserNetworkMediaType chosenMediaType;
    IOUserNetworkMediaType activeMediaType;
};

static RNDISDriverState gState = {};

static uint32_t RNDISHostToUSB32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

static uint32_t RNDISUSBToHost32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

static void ResetStateValues(void)
{
    bzero(&gState, sizeof(gState));
    gState.mtu = 1500U;
    gState.requestId = 1U;
    gState.packetFilter = kPacketFilterDirected | kPacketFilterBroadcast | kPacketFilterAllMulticast;
    gState.chosenMediaType = kIOUserNetworkMediaEthernetAuto;
    gState.activeMediaType = kIOUserNetworkMediaEthernet1000BaseT;
}

static uint32_t NextRequestId(void)
{
    uint32_t req = gState.requestId;
    gState.requestId = (gState.requestId + 1U);
    if (gState.requestId == 0U) {
        gState.requestId = 1U;
    }
    return req;
}

static kern_return_t CreateBuffer(uint64_t size, IOBufferMemoryDescriptor **buffer, uint8_t **bytes)
{
    IOBufferMemoryDescriptor *desc = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, size, 0, &desc);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    IOAddressSegment range = {};
    ret = desc->GetAddressRange(&range);
    if (ret != kIOReturnSuccess) {
        desc->release();
        return ret;
    }

    *buffer = desc;
    *bytes = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(range.address));
    return kIOReturnSuccess;
}

static void ReleaseUSBObjects(void)
{
    if (gState.bulkInPipe) {
        (void)gState.bulkInPipe->Abort(0, kIOReturnAborted, gState.driver);
    }
    OSSafeReleaseNULL(gState.bulkInPipe);
    OSSafeReleaseNULL(gState.bulkOutPipe);
    if (gState.dataInterface) {
        (void)gState.dataInterface->Close(gState.driver, 0);
    }
    if (gState.controlInterface) {
        (void)gState.controlInterface->Close(gState.driver, 0);
    }
    OSSafeReleaseNULL(gState.dataInterface);
    OSSafeReleaseNULL(gState.controlInterface);
    OSSafeReleaseNULL(gState.device);
    OSSafeReleaseNULL(gState.controlBuffer);
    gState.controlBufferBytes = nullptr;
    OSSafeReleaseNULL(gState.bulkInBuffer);
    gState.bulkInBytes = nullptr;
    OSSafeReleaseNULL(gState.bulkInAction);
}

static bool FindEndpointsForInterface(const IOUSBConfigurationDescriptor *config,
                                      uint8_t interfaceNumber,
                                      uint8_t *altSetting,
                                      uint8_t *bulkIn,
                                      uint8_t *bulkOut)
{
    if (!config) {
        return false;
    }

    const uint8_t *cursor = reinterpret_cast<const uint8_t *>(config);
    uint16_t totalLength = OSSwapLittleToHostInt16(config->wTotalLength);
    const uint8_t *end = cursor + totalLength;
    uint8_t currentInterface = 0xFF;
    uint8_t currentAlt = 0xFF;
    uint8_t foundIn = 0;
    uint8_t foundOut = 0;
    uint8_t foundAlt = 0;
    bool haveAlt = false;

    while ((cursor + 2U) <= end) {
        uint8_t length = cursor[0];
        uint8_t type = cursor[1];
        if (length == 0 || (cursor + length) > end) {
            break;
        }

        if (type == kIOUSBDescriptorTypeInterface && length >= sizeof(IOUSBInterfaceDescriptor)) {
            const IOUSBInterfaceDescriptor *iface = reinterpret_cast<const IOUSBInterfaceDescriptor *>(cursor);
            currentInterface = iface->bInterfaceNumber;
            currentAlt = iface->bAlternateSetting;
            if (currentInterface == interfaceNumber) {
                foundIn = 0;
                foundOut = 0;
                haveAlt = true;
            } else {
                haveAlt = false;
            }
        } else if (type == kIOUSBDescriptorTypeEndpoint && length >= sizeof(IOUSBEndpointDescriptor)) {
            if (haveAlt && currentInterface == interfaceNumber) {
                const IOUSBEndpointDescriptor *ep = reinterpret_cast<const IOUSBEndpointDescriptor *>(cursor);
                uint8_t transferType = ep->bmAttributes & 0x03U;
                if (transferType == kIOUSBEndpointDescriptorTransferTypeBulk) {
                    if (ep->bEndpointAddress & kIOUSBEndpointDescriptorDirectionIn) {
                        foundIn = ep->bEndpointAddress;
                    } else {
                        foundOut = ep->bEndpointAddress;
                    }
                    if (foundIn && foundOut) {
                        foundAlt = currentAlt;
                        break;
                    }
                }
            }
        }

        cursor += length;
    }

    if (foundIn && foundOut) {
        *bulkIn = foundIn;
        *bulkOut = foundOut;
        *altSetting = foundAlt;
        return true;
    }
    return false;
}

static void FreeConfigurationDescriptor(const IOUSBConfigurationDescriptor *config)
{
    if (!config) {
        return;
    }

    uint16_t length = OSSwapLittleToHostInt16(config->wTotalLength);
    if (length < sizeof(IOUSBConfigurationDescriptor)) {
        length = static_cast<uint16_t>(sizeof(IOUSBConfigurationDescriptor));
    }
    IOFree(const_cast<IOUSBConfigurationDescriptor *>(config), length);
}

static kern_return_t OpenInterfacesAndPipes(rndis_dk *driver, IOService *provider)
{
    IOUSBHostInterface *providerInterface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!providerInterface) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: provider is not IOUSBHostInterface");
        return kIOReturnUnsupported;
    }

    gState.driver = driver;
    gState.device = nullptr;

    IOUSBHostDevice *device = nullptr;
    kern_return_t ret = providerInterface->CopyDevice(&device);
    if (ret != kIOReturnSuccess || !device) {
        return ret != kIOReturnSuccess ? ret : kIOReturnNoDevice;
    }
    gState.device = device;

    const IOUSBConfigurationDescriptor *config = providerInterface->CopyConfigurationDescriptor();
    if (!config) {
        return kIOReturnNotFound;
    }

    uintptr_t iterator = 0;
    ret = gState.device->CreateInterfaceIterator(&iterator);
    if (ret != kIOReturnSuccess) {
        FreeConfigurationDescriptor(config);
        return ret;
    }

    IOUSBHostInterface *controlInterface = nullptr;
    IOUSBHostInterface *dataInterface = nullptr;
    uint8_t dataAlt = 0;
    uint8_t bulkIn = 0;
    uint8_t bulkOut = 0;
    bool haveDataEndpoints = false;

    while (true) {
        IOUSBHostInterface *candidate = nullptr;
        ret = gState.device->CopyInterface(iterator, &candidate);
        if (ret != kIOReturnSuccess || !candidate) {
            break;
        }

        const IOUSBInterfaceDescriptor *ifaceDesc = candidate->GetInterfaceDescriptor(config);
        if (!ifaceDesc) {
            OSSafeReleaseNULL(candidate);
            continue;
        }

        if ((ifaceDesc->bInterfaceClass == kRNDISControlClass) &&
            (ifaceDesc->bInterfaceSubClass == kRNDISControlSubClass) &&
            (ifaceDesc->bInterfaceProtocol == kRNDISControlProtocol)) {
            if (!controlInterface) {
                controlInterface = candidate;
                gState.controlInterfaceNumber = ifaceDesc->bInterfaceNumber;
                os_log(OS_LOG_DEFAULT,
                       "rndis-dk: found control iface num=%u class=0x%x subclass=0x%x proto=0x%x",
                       ifaceDesc->bInterfaceNumber,
                       ifaceDesc->bInterfaceClass,
                       ifaceDesc->bInterfaceSubClass,
                       ifaceDesc->bInterfaceProtocol);
                candidate = nullptr;
            }
        } else if (ifaceDesc->bInterfaceClass == kRNDISDataClass) {
            os_log(OS_LOG_DEFAULT,
                   "rndis-dk: candidate data iface num=%u class=0x%x subclass=0x%x proto=0x%x",
                   ifaceDesc->bInterfaceNumber,
                   ifaceDesc->bInterfaceClass,
                   ifaceDesc->bInterfaceSubClass,
                   ifaceDesc->bInterfaceProtocol);

            uint8_t altSetting = 0;
            uint8_t inAddr = 0;
            uint8_t outAddr = 0;
            if (FindEndpointsForInterface(config, ifaceDesc->bInterfaceNumber, &altSetting, &inAddr, &outAddr)) {
                os_log(OS_LOG_DEFAULT,
                       "rndis-dk: data iface endpoints alt=%u in=0x%x out=0x%x",
                       altSetting,
                       inAddr,
                       outAddr);

                if (dataInterface) {
                    OSSafeReleaseNULL(dataInterface);
                }
                dataInterface = candidate;
                candidate = nullptr;
                dataAlt = altSetting;
                bulkIn = inAddr;
                bulkOut = outAddr;
                haveDataEndpoints = true;
                gState.dataInterfaceNumber = ifaceDesc->bInterfaceNumber;
            } else if (!dataInterface) {
                dataInterface = candidate;
                candidate = nullptr;
                gState.dataInterfaceNumber = ifaceDesc->bInterfaceNumber;
            }
        }

        OSSafeReleaseNULL(candidate);
    }

    gState.device->DestroyInterfaceIterator(iterator);
    FreeConfigurationDescriptor(config);

    if (!controlInterface || !dataInterface || !haveDataEndpoints) {
        os_log(OS_LOG_DEFAULT,
               "rndis-dk: missing interfaces control=%p data=%p haveDataEndpoints=%d",
               controlInterface,
               dataInterface,
               haveDataEndpoints ? 1 : 0);
        OSSafeReleaseNULL(controlInterface);
        OSSafeReleaseNULL(dataInterface);
        return kIOReturnNotFound;
    }

    gState.controlInterface = controlInterface;
    gState.dataInterface = dataInterface;
    gState.dataAltSetting = dataAlt;
    gState.bulkInEndpointAddress = bulkIn;
    gState.bulkOutEndpointAddress = bulkOut;

    ret = gState.controlInterface->Open(driver, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = gState.dataInterface->Open(driver, 0, nullptr);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = gState.dataInterface->SelectAlternateSetting(gState.dataAltSetting);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    os_log(OS_LOG_DEFAULT,
           "rndis-dk: opened data iface num=%u alt=%u",
           gState.dataInterfaceNumber,
           gState.dataAltSetting);

    ret = gState.dataInterface->CopyPipe(gState.bulkInEndpointAddress, &gState.bulkInPipe);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    ret = gState.dataInterface->CopyPipe(gState.bulkOutEndpointAddress, &gState.bulkOutPipe);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    os_log(OS_LOG_DEFAULT,
           "rndis-dk: bulk pipes in=0x%x out=0x%x",
           gState.bulkInEndpointAddress,
           gState.bulkOutEndpointAddress);

    return kIOReturnSuccess;
}

static kern_return_t SendEncapsulatedCommand(const void *payload, uint32_t length)
{
    if (!gState.controlInterface || !payload || length == 0) {
        return kIOReturnBadArgument;
    }

    IOBufferMemoryDescriptor *buffer = nullptr;
    uint8_t *bytes = nullptr;
    kern_return_t ret = CreateBuffer(length, &buffer, &bytes);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    bcopy(payload, bytes, length);

    uint16_t bytesTransferred = 0;
    ret = gState.controlInterface->DeviceRequest(
        0x21,
        kCDCRequestSendEncapsulatedCommand,
        0,
        gState.controlInterfaceNumber,
        length,
        buffer,
        &bytesTransferred,
        kRNDISTimeoutMs);

    OSSafeReleaseNULL(buffer);

    if (ret != kIOReturnSuccess) {
        return ret;
    }

    return (bytesTransferred == length) ? kIOReturnSuccess : kIOReturnUnderrun;
}

static kern_return_t ReceiveEncapsulatedResponse(uint8_t **outBytes, uint32_t *outLength)
{
    if (!gState.controlInterface || !outBytes || !outLength) {
        return kIOReturnBadArgument;
    }

    if (!gState.controlBuffer) {
        kern_return_t ret = CreateBuffer(kControlBufferCapacity, &gState.controlBuffer, &gState.controlBufferBytes);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
    }

    uint16_t bytesTransferred = 0;
    kern_return_t ret = gState.controlInterface->DeviceRequest(
        0xA1,
        kCDCRequestGetEncapsulatedResponse,
        0,
        gState.controlInterfaceNumber,
        kControlBufferCapacity,
        gState.controlBuffer,
        &bytesTransferred,
        kRNDISTimeoutMs);

    if (ret != kIOReturnSuccess) {
        return ret;
    }

    *outBytes = gState.controlBufferBytes;
    *outLength = bytesTransferred;
    return kIOReturnSuccess;
}

static kern_return_t DoRNDISControlTransaction(const void *request,
                                               uint32_t requestLength,
                                               uint32_t expectedType,
                                               uint32_t expectedRequestId,
                                               uint8_t **response,
                                               uint32_t *responseLength)
{
    kern_return_t ret = SendEncapsulatedCommand(request, requestLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    for (uint32_t attempt = 0; attempt < kRNDISPollCount; ++attempt) {
        uint8_t *rx = nullptr;
        uint32_t rxLen = 0;
        ret = ReceiveEncapsulatedResponse(&rx, &rxLen);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        if (rxLen < sizeof(RNDISMessageHeader)) {
            continue;
        }

        const RNDISMessageHeader *header = reinterpret_cast<const RNDISMessageHeader *>(rx);
        uint32_t msgType = RNDISUSBToHost32(header->type);
        uint32_t msgLen = RNDISUSBToHost32(header->messageLength);
        if (msgLen < sizeof(RNDISMessageHeader) || msgLen > rxLen) {
            return kIOReturnBadMessageID;
        }

        if (msgType == kRNDISMsgIndicateStatus) {
            continue;
        }

        if (expectedType && msgType != expectedType) {
            continue;
        }

        if (expectedRequestId) {
            if (msgLen < 12) {
                return kIOReturnBadMessageID;
            }
            uint32_t respId = RNDISUSBToHost32(*reinterpret_cast<const uint32_t *>(rx + 8));
            if (respId != expectedRequestId) {
                continue;
            }
        }

        *response = rx;
        *responseLength = msgLen;
        return kIOReturnSuccess;
    }

    return kIOReturnTimeout;
}

static kern_return_t ParseQueryInformationBuffer(const uint8_t *response,
                                                 uint32_t responseLength,
                                                 const uint8_t **info,
                                                 uint32_t *infoLength)
{
    if (responseLength < sizeof(RNDISQueryComplete)) {
        return kIOReturnUnderrun;
    }

    const RNDISQueryComplete *complete = reinterpret_cast<const RNDISQueryComplete *>(response);
    uint32_t status = USBToHost32(complete->status);
    if (status != kRNDISStatusSuccess) {
        return kIOReturnNotResponding;
    }

    uint32_t len = USBToHost32(complete->infoLength);
    uint32_t offset = USBToHost32(complete->infoOffset);
    uint32_t messageLength = USBToHost32(complete->header.messageLength);
    if (messageLength > responseLength) {
        return kIOReturnUnderrun;
    }

    uint32_t start = 8 + offset;
    if ((start + len) <= messageLength) {
        *info = response + start;
        *infoLength = len;
        return kIOReturnSuccess;
    }

    uint32_t compatStart = 12 + offset;
    if ((compatStart + len) <= messageLength) {
        *info = response + compatStart;
        *infoLength = len;
        return kIOReturnSuccess;
    }

    return kIOReturnBadMessageID;
}

static kern_return_t RNDISInitialize(void)
{
    RNDISInitializeRequest request = {};
    request.header.type = RNDISHostToUSB32(kRNDISMsgInitialize);
    request.header.messageLength = RNDISHostToUSB32(sizeof(RNDISInitializeRequest));
    request.requestId = RNDISHostToUSB32(NextRequestId());
    request.majorVersion = RNDISHostToUSB32(1U);
    request.minorVersion = RNDISHostToUSB32(0U);
    request.maxTransferSize = RNDISHostToUSB32(0x4000U);

    uint8_t *response = nullptr;
    uint32_t responseLength = 0;
    kern_return_t ret = DoRNDISControlTransaction(&request, sizeof(request),
                                                  kRNDISMsgInitializeComplete,
                                                  RNDISUSBToHost32(request.requestId),
                                                  &response, &responseLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (responseLength < sizeof(RNDISInitializeComplete)) {
        return kIOReturnUnderrun;
    }

    const RNDISInitializeComplete *complete = reinterpret_cast<const RNDISInitializeComplete *>(response);
    uint32_t status = RNDISUSBToHost32(complete->status);
    if (status != kRNDISStatusSuccess) {
        return kIOReturnNotResponding;
    }

    gState.maxTransferSize = RNDISUSBToHost32(complete->maxTransferSize);
    return kIOReturnSuccess;
}

static kern_return_t RNDISQueryOID(uint32_t oid, uint8_t *buffer, uint32_t bufferCapacity, uint32_t *outLength)
{
    RNDISQueryRequest request = {};
    request.header.type = RNDISHostToUSB32(kRNDISMsgQuery);
    request.header.messageLength = RNDISHostToUSB32(sizeof(RNDISQueryRequest));
    request.requestId = RNDISHostToUSB32(NextRequestId());
    request.oid = RNDISHostToUSB32(oid);
    request.infoLength = 0;
    request.infoOffset = 0;
    request.deviceVcHandle = 0;

    uint8_t *response = nullptr;
    uint32_t responseLength = 0;
    kern_return_t ret = DoRNDISControlTransaction(&request, sizeof(request),
                                                  kRNDISMsgQueryComplete,
                                                  RNDISUSBToHost32(request.requestId),
                                                  &response, &responseLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    const uint8_t *info = nullptr;
    uint32_t infoLength = 0;
    ret = ParseQueryInformationBuffer(response, responseLength, &info, &infoLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (infoLength > bufferCapacity) {
        return kIOReturnNoSpace;
    }

    bcopy(info, buffer, infoLength);
    *outLength = infoLength;
    return kIOReturnSuccess;
}

static kern_return_t RNDISSetOID(uint32_t oid, const uint8_t *payload, uint32_t payloadLength)
{
    uint32_t infoOffset = 20U;
    uint32_t messageLength = sizeof(RNDISSetRequest) + payloadLength;

    uint8_t requestBuffer[sizeof(RNDISSetRequest) + 256] = {};
    if (payloadLength > 256U) {
        return kIOReturnNoSpace;
    }

    RNDISSetRequest *request = reinterpret_cast<RNDISSetRequest *>(requestBuffer);
    request->header.type = RNDISHostToUSB32(kRNDISMsgSet);
    request->header.messageLength = RNDISHostToUSB32(messageLength);
    request->requestId = RNDISHostToUSB32(NextRequestId());
    request->oid = RNDISHostToUSB32(oid);
    request->infoLength = RNDISHostToUSB32(payloadLength);
    request->infoOffset = RNDISHostToUSB32(infoOffset);
    request->deviceVcHandle = 0;

    bcopy(payload, requestBuffer + sizeof(RNDISSetRequest), payloadLength);

    uint8_t *response = nullptr;
    uint32_t responseLength = 0;
    kern_return_t ret = DoRNDISControlTransaction(requestBuffer, messageLength,
                                                  kRNDISMsgSetComplete,
                                                  RNDISUSBToHost32(request->requestId),
                                                  &response, &responseLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (responseLength < sizeof(RNDISSetComplete)) {
        return kIOReturnUnderrun;
    }

    const RNDISSetComplete *complete = reinterpret_cast<const RNDISSetComplete *>(response);
    uint32_t status = RNDISUSBToHost32(complete->status);
    return (status == kRNDISStatusSuccess) ? kIOReturnSuccess : kIOReturnNotResponding;
}

static kern_return_t RNDISKeepAlive(void)
{
    RNDISKeepAliveRequest request = {};
    request.header.type = RNDISHostToUSB32(kRNDISMsgKeepAlive);
    request.header.messageLength = RNDISHostToUSB32(sizeof(RNDISKeepAliveRequest));
    request.requestId = RNDISHostToUSB32(NextRequestId());

    uint8_t *response = nullptr;
    uint32_t responseLength = 0;
    kern_return_t ret = DoRNDISControlTransaction(&request, sizeof(request),
                                                  kRNDISMsgKeepAliveComplete,
                                                  RNDISUSBToHost32(request.requestId),
                                                  &response, &responseLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (responseLength < sizeof(RNDISKeepAliveComplete)) {
        return kIOReturnUnderrun;
    }

    const RNDISKeepAliveComplete *complete = reinterpret_cast<const RNDISKeepAliveComplete *>(response);
    uint32_t status = RNDISUSBToHost32(complete->status);
    return (status == kRNDISStatusSuccess) ? kIOReturnSuccess : kIOReturnNotResponding;
}

static void RNDISHalt(void)
{
    RNDISMessageHeader request = {};
    request.type = RNDISHostToUSB32(kRNDISMsgHalt);
    request.messageLength = RNDISHostToUSB32(sizeof(RNDISMessageHeader));
    (void)SendEncapsulatedCommand(&request, sizeof(request));
}

static kern_return_t UpdatePacketFilter(void)
{
    if (!gState.initialized) {
        return kIOReturnSuccess;
    }

    uint32_t filter = kPacketFilterDirected | kPacketFilterBroadcast;
    if (gState.allMulticast) {
        filter |= kPacketFilterAllMulticast;
    }
    if (gState.promiscuous) {
        filter |= kPacketFilterPromiscuous;
    }
    gState.packetFilter = filter;

    os_log(OS_LOG_DEFAULT, "rndis-dk: packet filter=0x%x", filter);

    uint32_t payload = RNDISHostToUSB32(filter);
    return RNDISSetOID(kOIDGenCurrentPacketFilter,
                       reinterpret_cast<uint8_t *>(&payload),
                       sizeof(payload));
}

static kern_return_t InitializeRNDISDevice(void)
{
    os_log(OS_LOG_DEFAULT, "rndis-dk: initializing RNDIS control plane");
    kern_return_t ret = RNDISInitialize();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RNDIS initialize failed ret=0x%x", ret);
        return ret;
    }

    uint8_t mac[8] = {};
    uint32_t macLen = 0;
    ret = RNDISQueryOID(kOID8023CurrentAddress, mac, sizeof(mac), &macLen);
    if (ret != kIOReturnSuccess || macLen < 6) {
        ret = RNDISQueryOID(kOID8023PermanentAddress, mac, sizeof(mac), &macLen);
    }

    if (ret != kIOReturnSuccess || macLen < 6) {
        return ret != kIOReturnSuccess ? ret : kIOReturnUnderrun;
    }

    bzero(&gState.macAddress, sizeof(gState.macAddress));
    bcopy(mac, gState.macAddress.octet, 6);

        os_log(OS_LOG_DEFAULT,
            "rndis-dk: MAC %02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    gState.initialized = true;
    ret = UpdatePacketFilter();
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    return RNDISKeepAlive();
}

static kern_return_t WrapEthernetFrame(const uint8_t *frame,
                                       uint32_t frameLength,
                                       uint8_t *outBuffer,
                                       uint32_t outCapacity,
                                       uint32_t *outLength)
{
    if (!frame || !outBuffer || !outLength) {
        return kIOReturnBadArgument;
    }

    uint32_t copyLength = frameLength;
    if (frameLength < kEthernetMinFrameLength) {
        frameLength = kEthernetMinFrameLength;
    }

    uint32_t messageLength = kRNDISPacketHeaderLength + frameLength;
    if (messageLength > outCapacity) {
        return kIOReturnNoSpace;
    }

    bzero(outBuffer, kRNDISPacketHeaderLength);
    RNDISPacketMessage *packet = reinterpret_cast<RNDISPacketMessage *>(outBuffer);
    packet->header.type = RNDISHostToUSB32(kRNDISMsgPacket);
    packet->header.messageLength = RNDISHostToUSB32(messageLength);
    packet->dataOffset = RNDISHostToUSB32(36U);
    packet->dataLength = RNDISHostToUSB32(frameLength);

    bcopy(frame, outBuffer + kRNDISPacketHeaderLength, copyLength);
    if (copyLength < frameLength) {
        bzero(outBuffer + kRNDISPacketHeaderLength + copyLength, frameLength - copyLength);
    }
    *outLength = messageLength;
    return kIOReturnSuccess;
}

static kern_return_t SendBulkOutData(const uint8_t *frame, uint32_t frameLength)
{
    if (!gState.bulkOutPipe || !frame || frameLength == 0) {
        return kIOReturnBadArgument;
    }

    uint8_t buffer[kMaxWrapBufferSize] = {};
    uint32_t wrappedLength = 0;
    kern_return_t ret = WrapEthernetFrame(frame, frameLength, buffer, sizeof(buffer), &wrappedLength);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    IOBufferMemoryDescriptor *desc = nullptr;
    uint8_t *bytes = nullptr;
    ret = CreateBuffer(wrappedLength, &desc, &bytes);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    bcopy(buffer, bytes, wrappedLength);

    uint32_t bytesTransferred = 0;
    ret = gState.bulkOutPipe->IO(desc, wrappedLength, &bytesTransferred, kRNDISTimeoutMs);
    OSSafeReleaseNULL(desc);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    return (bytesTransferred == wrappedLength) ? kIOReturnSuccess : kIOReturnUnderrun;
}

static void HandleReceivedFrames(const uint8_t *data, uint32_t length)
{
    uint32_t cursor = 0;
    while ((cursor + 8U) <= length) {
        const uint8_t *base = data + cursor;
        uint32_t msgType = RNDISUSBToHost32(*reinterpret_cast<const uint32_t *>(base));
        uint32_t msgLen = RNDISUSBToHost32(*reinterpret_cast<const uint32_t *>(base + 4));
        if (msgType != kRNDISMsgPacket || msgLen < kRNDISPacketHeaderLength) {
            break;
        }
        if ((cursor + msgLen) > length) {
            break;
        }

        uint32_t dataOffset = RNDISUSBToHost32(*reinterpret_cast<const uint32_t *>(base + 8));
        uint32_t dataLen = RNDISUSBToHost32(*reinterpret_cast<const uint32_t *>(base + 12));
        uint32_t dataStart = 8U + dataOffset;
        if (dataStart < kRNDISPacketHeaderLength || (dataStart + dataLen) > msgLen) {
            break;
        }

        if (gState.rxsQueue && gState.rxcQueue) {
            IOUserNetworkPacket *packets[4] = {};
            uint32_t available = gState.rxsQueue->DequeuePackets(packets, 4);
            if (available > 0) {
                IOUserNetworkPacket *packet = packets[0];
                uint8_t *pktData = reinterpret_cast<uint8_t *>(packet->getDataVirtualAddress());
                uint64_t pktOffset = packet->getDataOffset();
                uint8_t *dst = pktData + pktOffset;
                uint32_t copyLen = dataLen;
                bcopy(base + dataStart, dst, copyLen);

                (void)packet->setDataLength(copyLen);
                (void)packet->SetLinkHeaderLength(14);

                if (gState.rxcQueue->EnqueuePacket(packet) != kIOReturnSuccess) {
                    gState.pool->DeallocatePacket(packet);
                }

                for (uint32_t i = 1; i < available; ++i) {
                    gState.pool->DeallocatePacket(packets[i]);
                }
            }
        }

        cursor += msgLen;
    }
}

static kern_return_t StartBulkInListening(void)
{
    if (!gState.bulkInPipe) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN pipe missing");
        return kIOReturnNotFound;
    }

    if (!gState.bulkInBuffer) {
        kern_return_t ret = CreateBuffer(kBulkInBufferSize, &gState.bulkInBuffer, &gState.bulkInBytes);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
    }

    if (!gState.bulkInAction) {
        if (!gState.driver) {
            return kIOReturnNotReady;
        }
        kern_return_t ret = gState.driver->CreateActionBulkInComplete(0, &gState.bulkInAction);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
    }

    kern_return_t ret = gState.bulkInPipe->AsyncIO(gState.bulkInBuffer, kBulkInBufferSize,
                                                   gState.bulkInAction, kRNDISTimeoutMs);
    if (ret == kIOReturnSuccess) {
        gState.bulkInListening = true;
    }
    os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN AsyncIO ret=0x%x", ret);
    return ret;
}

static void StopBulkInListening(void)
{
    if (!gState.bulkInPipe) {
        return;
    }

    (void)gState.bulkInPipe->Abort(0, kIOReturnAborted, gState.driver);
    gState.bulkInListening = false;
    os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN stopped");
}

} // namespace

kern_return_t
IMPL(rndis_dk, Start)
{
    os_log(OS_LOG_DEFAULT, "rndis-dk: Start called");
    IOUserNetworkPacketBufferPoolOptions poolOptions = {};
    IODataQueueDispatchSource *txDataQueue = nullptr;
    IOUserNetworkPacketQueue *queues[4] = {};
    static const IOUserNetworkMediaType mediaTable[] = {
        kIOUserNetworkMediaEthernetAuto,
        kIOUserNetworkMediaEthernet100BaseTX,
        kIOUserNetworkMediaEthernet1000BaseT
    };

    kern_return_t ret = kIOReturnSuccess;
    bool superStarted = false;

    static const bool kCallSuperAtStart = true;
    if (kCallSuperAtStart) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: Start entering super::Start(provider)");
        ret = super::Start(provider, SUPERDISPATCH);
        os_log(OS_LOG_DEFAULT, "rndis-dk: super::Start(provider) returned ret=0x%x", ret);
        if (ret != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "rndis-dk: super::Start(provider) failed ret=0x%x", ret);
            return ret;
        }
        superStarted = true;
    }

    ResetStateValues();

    ret = CopyDispatchQueue("Default", &gState.dispatchQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: CopyDispatchQueue failed ret=0x%x", ret);
        goto fail;
    }

    ret = OpenInterfacesAndPipes(this, provider);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: OpenInterfacesAndPipes failed ret=0x%x", ret);
        goto fail;
    }

    ret = InitializeRNDISDevice();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RNDIS init failed ret=0x%x", ret);
        goto fail;
    }

    poolOptions.packetCount = 64;
    poolOptions.bufferCount = 64;
    poolOptions.bufferSize = 16 * 1024;
    poolOptions.maxBuffersPerPacket = 1;
    poolOptions.poolFlags = PoolFlagMapToDext;
    poolOptions.dmaSpecification.maxAddressBits = 64;

    ret = IOUserNetworkPacketBufferPool::CreateWithOptions(this, "rndis-dk", &poolOptions, &gState.pool);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: CreateWithOptions failed ret=0x%x", ret);
        goto fail;
    }

    ret = CreateActionTxPacketAvailable(0, &gState.txPacketAction);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: CreateActionTxPacketAvailable failed ret=0x%x", ret);
        goto fail;
    }

    ret = IOUserNetworkTxSubmissionQueue::Create(gState.pool, this, 8, 0, gState.dispatchQueue, &gState.txsQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: TxSubmissionQueue::Create failed ret=0x%x", ret);
        goto fail;
    }

    ret = gState.txsQueue->CopyDataQueue(&txDataQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: CopyDataQueue failed ret=0x%x", ret);
        goto fail;
    }

    ret = txDataQueue->SetDataAvailableHandler(gState.txPacketAction);
    OSSafeReleaseNULL(txDataQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: SetDataAvailableHandler failed ret=0x%x", ret);
        goto fail;
    }

    ret = IOUserNetworkTxCompletionQueue::Create(gState.pool, this, 8, 0, gState.dispatchQueue, &gState.txcQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: TxCompletionQueue::Create failed ret=0x%x", ret);
        goto fail;
    }

    ret = IOUserNetworkRxSubmissionQueue::Create(gState.pool, this, 8, 0, gState.dispatchQueue, &gState.rxsQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RxSubmissionQueue::Create failed ret=0x%x", ret);
        goto fail;
    }

    ret = IOUserNetworkRxCompletionQueue::Create(gState.pool, this, 8, 0, gState.dispatchQueue, &gState.rxcQueue);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RxCompletionQueue::Create failed ret=0x%x", ret);
        goto fail;
    }

    ret = ReportAvailableMediaTypes(mediaTable, sizeof(mediaTable) / sizeof(mediaTable[0]));
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: ReportAvailableMediaTypes failed ret=0x%x", ret);
        goto fail;
    }

    (void)SetTxPacketHeadroom(0);
    (void)SetTxPacketTailroom(0);
    (void)SetWakeOnMagicPacketSupport(false);

    queues[0] = gState.txsQueue;
    queues[1] = gState.txcQueue;
    queues[2] = gState.rxsQueue;
    queues[3] = gState.rxcQueue;
    ret = RegisterEthernetInterface(gState.macAddress, gState.pool, queues, 4);
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RegisterEthernetInterface failed ret=0x%x", ret);
        goto fail;
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: RegisterService failed ret=0x%x", ret);
        goto fail;
    }

    ret = StartBulkInListening();
    if (ret != kIOReturnSuccess) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN start failed ret=0x%x", ret);
        goto fail;
    }

    os_log(OS_LOG_DEFAULT,
           "rndis-dk: initialized control-if=%u data-if=%u bulkIn=0x%x bulkOut=0x%x maxTransfer=%u",
           gState.controlInterfaceNumber,
           gState.dataInterfaceNumber,
           gState.bulkInEndpointAddress,
           gState.bulkOutEndpointAddress,
           gState.maxTransferSize);

    return kIOReturnSuccess;

fail:
    os_log(OS_LOG_DEFAULT, "rndis-dk: Start failed ret=0x%x", ret);
    StopBulkInListening();
    OSSafeReleaseNULL(gState.rxcQueue);
    OSSafeReleaseNULL(gState.rxsQueue);
    OSSafeReleaseNULL(gState.txcQueue);
    OSSafeReleaseNULL(gState.txsQueue);
    OSSafeReleaseNULL(gState.txPacketAction);
    OSSafeReleaseNULL(gState.pool);
    OSSafeReleaseNULL(gState.dispatchQueue);
    ReleaseUSBObjects();
    if (superStarted) {
        (void)super::Stop(provider, SUPERDISPATCH);
    }
    return ret;
}

kern_return_t
IMPL(rndis_dk, Stop)
{
    os_log(OS_LOG_DEFAULT, "rndis-dk: Stop called");
    StopBulkInListening();

    if (gState.interfaceEnabled) {
        (void)ReportLinkStatus(kIOUserNetworkLinkStatusInactive, gState.activeMediaType);
    }

    if (gState.initialized) {
        RNDISHalt();
    }

    OSSafeReleaseNULL(gState.rxcQueue);
    OSSafeReleaseNULL(gState.rxsQueue);
    OSSafeReleaseNULL(gState.txcQueue);
    OSSafeReleaseNULL(gState.txsQueue);
    OSSafeReleaseNULL(gState.txPacketAction);
    OSSafeReleaseNULL(gState.pool);
    OSSafeReleaseNULL(gState.dispatchQueue);
    ReleaseUSBObjects();

    return super::Stop(provider, SUPERDISPATCH);
}

void
IMPL(rndis_dk, TxPacketAvailable)
{
    if (!gState.txsQueue || !gState.txcQueue) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: tx callback without queues");
        return;
    }

    IOUserNetworkPacket *packets[8] = {};
    uint32_t count = gState.txsQueue->DequeuePackets(packets, 8);
    if (kVerboseDataPathLogs && count > 0) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: tx dequeued %u packet(s)", count);
    }
    for (uint32_t i = 0; i < count; ++i) {
        IOUserNetworkPacket *packet = packets[i];
        uint8_t *data = reinterpret_cast<uint8_t *>(packet->getDataVirtualAddress());
        uint64_t offset = packet->getDataOffset();
        uint32_t length = packet->getDataLength();
        const uint8_t *frame = data + offset;

        kern_return_t ret = SendBulkOutData(frame, length);
        if (ret != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "rndis-dk: bulk out failed ret=0x%x", ret);
        }

        if (gState.txcQueue->EnqueuePacket(packet) != kIOReturnSuccess) {
            gState.pool->DeallocatePacket(packet);
        }
    }
}

void
IMPL(rndis_dk, BulkInComplete)
{
    if (!gState.bulkInListening) {
        return;
    }

    if (status == kIOReturnAborted) {
        return;
    }

    if (status == kIOReturnSuccess && actualByteCount > 0) {
        if (kVerboseDataPathLogs) {
            os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN received %u byte(s)", actualByteCount);
        }
        HandleReceivedFrames(gState.bulkInBytes, actualByteCount);
    } else if (status != kIOReturnSuccess && status != kIOReturnTimeout) {
        os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN completion status=0x%x", status);
    }

    if (gState.bulkInListening && gState.bulkInPipe) {
        kern_return_t ret = gState.bulkInPipe->AsyncIO(gState.bulkInBuffer, kBulkInBufferSize,
                                                       gState.bulkInAction, kRNDISTimeoutMs);
        if (ret != kIOReturnSuccess) {
            os_log(OS_LOG_DEFAULT, "rndis-dk: bulk IN resubmit failed ret=0x%x", ret);
        }
    }
}

kern_return_t
IMPL(rndis_dk, SetInterfaceEnable)
{
    kern_return_t ret = kIOReturnSuccess;
    os_log(OS_LOG_DEFAULT, "rndis-dk: interface enable=%d", enable ? 1 : 0);

    if (enable) {
        if (gState.txcQueue) {
            ret = gState.txcQueue->SetEnable(true);
            if (ret != kIOReturnSuccess) {
                return ret;
            }
        }
        if (gState.txsQueue) {
            ret = gState.txsQueue->SetEnable(true);
            if (ret != kIOReturnSuccess) {
                return ret;
            }
        }
        if (gState.rxcQueue) {
            ret = gState.rxcQueue->SetEnable(true);
            if (ret != kIOReturnSuccess) {
                return ret;
            }
        }
        if (gState.rxsQueue) {
            ret = gState.rxsQueue->SetEnable(true);
            if (ret != kIOReturnSuccess) {
                return ret;
            }
        }

        ret = ReportLinkStatus(kIOUserNetworkLinkStatusActive, gState.activeMediaType);
        if (ret != kIOReturnSuccess) {
            return ret;
        }

        gState.interfaceEnabled = true;
        os_log(OS_LOG_DEFAULT, "rndis-dk: link active");
    } else {
        if (gState.txcQueue) {
            (void)gState.txcQueue->SetEnable(false);
        }
        if (gState.txsQueue) {
            (void)gState.txsQueue->SetEnable(false);
        }
        if (gState.rxcQueue) {
            (void)gState.rxcQueue->SetEnable(false);
        }
        if (gState.rxsQueue) {
            (void)gState.rxsQueue->SetEnable(false);
        }

        (void)ReportLinkStatus(kIOUserNetworkLinkStatusInactive, gState.activeMediaType);
        gState.interfaceEnabled = false;
        os_log(OS_LOG_DEFAULT, "rndis-dk: link inactive");
    }

    return ret;
}

kern_return_t
IMPL(rndis_dk, SetPromiscuousModeEnable)
{
    gState.promiscuous = enable;
    os_log(OS_LOG_DEFAULT, "rndis-dk: promiscuous=%d", enable ? 1 : 0);
    return UpdatePacketFilter();
}

kern_return_t
IMPL(rndis_dk, SetMulticastAddresses)
{
    (void)addresses;
    (void)count;
    return kIOReturnSuccess;
}

kern_return_t
IMPL(rndis_dk, SetAllMulticastModeEnable)
{
    gState.allMulticast = enable;
    os_log(OS_LOG_DEFAULT, "rndis-dk: all-multicast=%d", enable ? 1 : 0);
    return UpdatePacketFilter();
}

kern_return_t
IMPL(rndis_dk, SelectMediaType)
{
    os_log(OS_LOG_DEFAULT, "rndis-dk: select media=0x%x", mediaType);
    gState.chosenMediaType = mediaType;
    if (gState.chosenMediaType == kIOUserNetworkMediaEthernetAuto) {
        gState.activeMediaType = kIOUserNetworkMediaEthernet1000BaseT;
    } else {
        gState.activeMediaType = gState.chosenMediaType;
    }

    if (gState.interfaceEnabled) {
        return ReportLinkStatus(kIOUserNetworkLinkStatusActive, gState.activeMediaType);
    }
    return kIOReturnSuccess;
}

kern_return_t
IMPL(rndis_dk, SetMTU)
{
    gState.mtu = mtu;
    os_log(OS_LOG_DEFAULT, "rndis-dk: mtu=%u", mtu);
    return kIOReturnSuccess;
}

kern_return_t
IMPL(rndis_dk, GetMaxTransferUnit)
{
    if (!mtu) {
        return kIOReturnBadArgument;
    }

    *mtu = gState.mtu;
    return kIOReturnSuccess;
}
