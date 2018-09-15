/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "CompositionTest"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <gui/IProducerListener.h>
#include <log/log.h>
#include <system/window.h>
#include <utils/String8.h>

#include "BufferQueueLayer.h"
#include "ColorLayer.h"
#include "Layer.h"

#include "TestableSurfaceFlinger.h"
#include "mock/DisplayHardware/MockComposer.h"
#include "mock/DisplayHardware/MockDisplaySurface.h"
#include "mock/MockDispSync.h"
#include "mock/MockEventControlThread.h"
#include "mock/MockEventThread.h"
#include "mock/MockMessageQueue.h"
#include "mock/RenderEngine/MockRenderEngine.h"

namespace android {
namespace {

using testing::_;
using testing::ByMove;
using testing::DoAll;
using testing::IsNull;
using testing::Mock;
using testing::NotNull;
using testing::Ref;
using testing::Return;
using testing::ReturnRef;
using testing::SetArgPointee;

using android::Hwc2::Error;
using android::Hwc2::IComposer;
using android::Hwc2::IComposerClient;
using android::Hwc2::Transform;

using FakeHwcDisplayInjector = TestableSurfaceFlinger::FakeHwcDisplayInjector;
using FakeDisplayDeviceInjector = TestableSurfaceFlinger::FakeDisplayDeviceInjector;

constexpr hwc2_display_t HWC_DISPLAY = FakeHwcDisplayInjector::DEFAULT_HWC_DISPLAY_ID;
constexpr hwc2_layer_t HWC_LAYER = 5000;
constexpr Transform DEFAULT_TRANSFORM = static_cast<Transform>(0);

constexpr int DEFAULT_DISPLAY_WIDTH = 1920;
constexpr int DEFAULT_DISPLAY_HEIGHT = 1024;

constexpr int DEFAULT_CONFIG_ID = 0;
constexpr int DEFAULT_TEXTURE_ID = 6000;
constexpr int DEFAULT_LAYER_STACK = 7000;

constexpr int DEFAULT_DISPLAY_MAX_LUMINANCE = 500;

constexpr int DEFAULT_SIDEBAND_STREAM = 51;

class CompositionTest : public testing::Test {
public:
    CompositionTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Setting up for %s.%s\n", test_info->test_case_name(), test_info->name());

        mFlinger.mutableEventControlThread().reset(mEventControlThread);
        mFlinger.mutableEventThread().reset(mEventThread);
        mFlinger.mutableEventQueue().reset(mMessageQueue);

        mFlinger.mutablePrimaryDispSync().reset(mPrimaryDispSync);
        EXPECT_CALL(*mPrimaryDispSync, computeNextRefresh(0)).WillRepeatedly(Return(0));
        EXPECT_CALL(*mPrimaryDispSync, getPeriod())
                .WillRepeatedly(Return(FakeHwcDisplayInjector::DEFAULT_REFRESH_RATE));

        mFlinger.setupRenderEngine(std::unique_ptr<renderengine::RenderEngine>(mRenderEngine));
        setupComposer(0);
    }

    ~CompositionTest() {
        const ::testing::TestInfo* const test_info =
                ::testing::UnitTest::GetInstance()->current_test_info();
        ALOGD("**** Tearing down after %s.%s\n", test_info->test_case_name(), test_info->name());
    }

    void setupComposer(int virtualDisplayCount) {
        mComposer = new Hwc2::mock::Composer();
        EXPECT_CALL(*mComposer, getCapabilities())
                .WillOnce(Return(std::vector<IComposer::Capability>()));
        EXPECT_CALL(*mComposer, getMaxVirtualDisplayCount()).WillOnce(Return(virtualDisplayCount));
        mFlinger.setupComposer(std::unique_ptr<Hwc2::Composer>(mComposer));

        Mock::VerifyAndClear(mComposer);
    }

    void setupForceGeometryDirty() {
        // TODO: This requires the visible region and other related
        // state to be set, and is problematic for BufferLayers since they are
        // not visible without a buffer (and setting up a buffer looks like a
        // pain)
        // mFlinger.mutableVisibleRegionsDirty() = true;

        mFlinger.mutableGeometryInvalid() = true;
    }

    template <typename Case>
    void displayRefreshCompositionDirtyGeometry();

    template <typename Case>
    void displayRefreshCompositionDirtyFrame();

    template <typename Case>
    void captureScreenComposition();

    std::unordered_set<HWC2::Capability> mDefaultCapabilities = {HWC2::Capability::SidebandStream};

    TestableSurfaceFlinger mFlinger;
    sp<DisplayDevice> mDisplay;
    sp<DisplayDevice> mExternalDisplay;
    sp<mock::DisplaySurface> mDisplaySurface = new mock::DisplaySurface();
    renderengine::mock::Surface* mRenderSurface = new renderengine::mock::Surface();

    mock::EventThread* mEventThread = new mock::EventThread();
    mock::EventControlThread* mEventControlThread = new mock::EventControlThread();

    Hwc2::mock::Composer* mComposer = nullptr;
    renderengine::mock::RenderEngine* mRenderEngine = new renderengine::mock::RenderEngine();
    mock::MessageQueue* mMessageQueue = new mock::MessageQueue();
    mock::DispSync* mPrimaryDispSync = new mock::DispSync();
    renderengine::mock::Image* mReImage = new renderengine::mock::Image();
    renderengine::mock::Framebuffer* mReFrameBuffer = new renderengine::mock::Framebuffer();

    sp<Fence> mClientTargetAcquireFence = Fence::NO_FENCE;

    sp<GraphicBuffer> mCaptureScreenBuffer;
};

template <typename LayerCase>
void CompositionTest::displayRefreshCompositionDirtyGeometry() {
    setupForceGeometryDirty();
    LayerCase::setupForDirtyGeometry(this);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.onMessageReceived(MessageQueue::INVALIDATE);
    mFlinger.onMessageReceived(MessageQueue::REFRESH);

    LayerCase::cleanup(this);
}

template <typename LayerCase>
void CompositionTest::displayRefreshCompositionDirtyFrame() {
    LayerCase::setupForDirtyFrame(this);

    // --------------------------------------------------------------------
    // Invocation

    mFlinger.onMessageReceived(MessageQueue::INVALIDATE);
    mFlinger.onMessageReceived(MessageQueue::REFRESH);

    LayerCase::cleanup(this);
}

template <typename LayerCase>
void CompositionTest::captureScreenComposition() {
    LayerCase::setupForScreenCapture(this);

    const Rect sourceCrop(0, 0, DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT);
    constexpr int32_t minLayerZ = -1;
    constexpr int32_t maxLayerZ = 1000;
    constexpr bool useIdentityTransform = true;
    constexpr bool forSystem = true;

    DisplayRenderArea renderArea(mDisplay, sourceCrop, DEFAULT_DISPLAY_WIDTH,
                                 DEFAULT_DISPLAY_HEIGHT, ui::Transform::ROT_0);

    auto traverseLayers = [this](const LayerVector::Visitor& visitor) {
        return mFlinger.traverseLayersInDisplay(mDisplay, minLayerZ, maxLayerZ, visitor);
    };

    // TODO: Eliminate expensive/real allocation if possible.
    const uint32_t usage = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN |
            GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    mCaptureScreenBuffer = new GraphicBuffer(renderArea.getReqWidth(), renderArea.getReqHeight(),
                                             HAL_PIXEL_FORMAT_RGBA_8888, 1, usage, "screenshot");

    int fd = -1;
    status_t result =
            mFlinger.captureScreenImplLocked(renderArea, traverseLayers, mCaptureScreenBuffer.get(),
                                             useIdentityTransform, forSystem, &fd);
    if (fd >= 0) {
        close(fd);
    }

    EXPECT_EQ(NO_ERROR, result);

    LayerCase::cleanup(this);
}

/* ------------------------------------------------------------------------
 * Variants for each display configuration which can be tested
 */

template <typename Derived>
struct BaseDisplayVariant {
    static constexpr bool IS_SECURE = true;
    static constexpr int INIT_POWER_MODE = HWC_POWER_MODE_NORMAL;

    static void setupPreconditions(CompositionTest* test) {
        FakeHwcDisplayInjector(DisplayDevice::DISPLAY_PRIMARY, HWC2::DisplayType::Physical)
                .setCapabilities(&test->mDefaultCapabilities)
                .inject(&test->mFlinger, test->mComposer);

        test->mDisplay = FakeDisplayDeviceInjector(test->mFlinger, DisplayDevice::DISPLAY_PRIMARY,
                                                   DisplayDevice::DISPLAY_PRIMARY)
                                 .setDisplaySize(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT)
                                 .setDisplaySurface(test->mDisplaySurface)
                                 .setRenderSurface(std::unique_ptr<renderengine::Surface>(
                                         test->mRenderSurface))
                                 .setSecure(Derived::IS_SECURE)
                                 .setPowerMode(Derived::INIT_POWER_MODE)
                                 .inject();
        test->mDisplay->setLayerStack(DEFAULT_LAYER_STACK);
    }

    template <typename Case>
    static void setupCommonCompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setColorTransform(HWC_DISPLAY, _, Hwc2::ColorTransform::IDENTITY))
                .Times(1);
        EXPECT_CALL(*test->mComposer, presentOrValidateDisplay(HWC_DISPLAY, _, _, _, _)).Times(1);
        EXPECT_CALL(*test->mComposer, getDisplayRequests(HWC_DISPLAY, _, _, _)).Times(1);
        EXPECT_CALL(*test->mComposer, acceptDisplayChanges(HWC_DISPLAY)).Times(1);
        EXPECT_CALL(*test->mComposer, presentDisplay(HWC_DISPLAY, _)).Times(1);
        EXPECT_CALL(*test->mComposer, getReleaseFences(HWC_DISPLAY, _, _)).Times(1);

        EXPECT_CALL(*test->mRenderEngine, useNativeFenceSync()).WillRepeatedly(Return(true));
        EXPECT_CALL(*test->mRenderEngine, checkErrors()).WillRepeatedly(Return());
        EXPECT_CALL(*test->mRenderEngine, isCurrent()).WillRepeatedly(Return(true));

        EXPECT_CALL(*test->mRenderEngine, setCurrentSurface(Ref(*test->mRenderSurface)))
                .WillOnce(Return(true));
        EXPECT_CALL(*test->mRenderEngine,
                    setViewportAndProjection(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT,
                                             Rect(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT),
                                             ui::Transform::ROT_0))
                .Times(1);

        EXPECT_CALL(*test->mDisplaySurface, onFrameCommitted()).Times(1);
        EXPECT_CALL(*test->mDisplaySurface, advanceFrame()).Times(1);

        Case::CompositionType::setupHwcSetCallExpectations(test);
        Case::CompositionType::setupHwcGetCallExpectations(test);
    }

    template <typename Case>
    static void setupCommonScreensCaptureCallExpectations(CompositionTest* test) {
        // Called once with a non-null value to set a framebuffer, and then
        // again with nullptr to clear it.
        EXPECT_CALL(*test->mReFrameBuffer, setNativeWindowBuffer(NotNull())).WillOnce(Return(true));
        EXPECT_CALL(*test->mReFrameBuffer, setNativeWindowBuffer(IsNull())).WillOnce(Return(true));

        EXPECT_CALL(*test->mRenderEngine, checkErrors()).WillRepeatedly(Return());
        EXPECT_CALL(*test->mRenderEngine, createFramebuffer())
                .WillOnce(Return(
                        ByMove(std::unique_ptr<renderengine::Framebuffer>(test->mReFrameBuffer))));
        EXPECT_CALL(*test->mRenderEngine, bindFrameBuffer(test->mReFrameBuffer)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, unbindFrameBuffer(test->mReFrameBuffer)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, clearWithColor(0, 0, 0, 1)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, flush()).WillOnce(Return(ByMove(base::unique_fd())));
        EXPECT_CALL(*test->mRenderEngine, finish()).WillOnce(Return(true));

        EXPECT_CALL(*test->mRenderEngine, setOutputDataSpace(ui::Dataspace::SRGB)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setDisplayMaxLuminance(DEFAULT_DISPLAY_MAX_LUMINANCE))
                .Times(1);
        // This expectation retires on saturation as setViewportAndProjection is
        // called an extra time for the code path this setup is for.
        // TODO: Investigate this extra call
        EXPECT_CALL(*test->mRenderEngine,
                    setViewportAndProjection(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT,
                                             Rect(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT),
                                             ui::Transform::ROT_0))
                .Times(1)
                .RetiresOnSaturation();
        EXPECT_CALL(*test->mRenderEngine, disableTexturing()).Times(1);
    }

    static void setupNonEmptyFrameCompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mDisplaySurface, beginFrame(true)).Times(1);
    }

    static void setupEmptyFrameCompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mDisplaySurface, beginFrame(false)).Times(1);
    }

    static void setupHwcCompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mDisplaySurface, prepareFrame(DisplaySurface::COMPOSITION_HWC)).Times(1);

        EXPECT_CALL(*test->mRenderEngine, disableScissor()).Times(1);
    }

    static void setupRECompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mDisplaySurface, prepareFrame(DisplaySurface::COMPOSITION_GLES))
                .Times(1);
        EXPECT_CALL(*test->mDisplaySurface, getClientTargetAcquireFence())
                .WillRepeatedly(ReturnRef(test->mClientTargetAcquireFence));

        EXPECT_CALL(*test->mRenderEngine, setOutputDataSpace(ui::Dataspace::UNKNOWN)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setDisplayMaxLuminance(DEFAULT_DISPLAY_MAX_LUMINANCE))
                .Times(1);
        EXPECT_CALL(*test->mRenderEngine, setupColorTransform(_)).Times(2);
        // These expectations retire on saturation as the code path these
        // expectations are for appears to make an extra call to them.
        // TODO: Investigate this extra call
        EXPECT_CALL(*test->mRenderEngine,
                    setViewportAndProjection(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT,
                                             Rect(DEFAULT_DISPLAY_WIDTH, DEFAULT_DISPLAY_HEIGHT),
                                             ui::Transform::ROT_0))
                .Times(1)
                .RetiresOnSaturation();
        EXPECT_CALL(*test->mRenderEngine, setCurrentSurface(Ref(*test->mRenderSurface)))
                .WillOnce(Return(true))
                .RetiresOnSaturation();
        EXPECT_CALL(*test->mRenderSurface, swapBuffers()).Times(1);
    }

    template <typename Case>
    static void setupRELayerCompositionCallExpectations(CompositionTest* test) {
        Case::Layer::setupRECompositionCallExpectations(test);
    }

    template <typename Case>
    static void setupRELayerScreenshotCompositionCallExpectations(CompositionTest* test) {
        Case::Layer::setupREScreenshotCompositionCallExpectations(test);

        EXPECT_CALL(*test->mRenderEngine, isCurrent()).WillRepeatedly(Return(true));
    }
};

struct DefaultDisplaySetupVariant : public BaseDisplayVariant<DefaultDisplaySetupVariant> {};

struct InsecureDisplaySetupVariant : public BaseDisplayVariant<InsecureDisplaySetupVariant> {
    static constexpr bool IS_SECURE = false;

    template <typename Case>
    static void setupRELayerCompositionCallExpectations(CompositionTest* test) {
        Case::Layer::setupInsecureRECompositionCallExpectations(test);

        // TODO: Investigate this extra call
        EXPECT_CALL(*test->mRenderEngine, disableScissor()).Times(1);
    }

    template <typename Case>
    static void setupRELayerScreenshotCompositionCallExpectations(CompositionTest* test) {
        Case::Layer::setupInsecureREScreenshotCompositionCallExpectations(test);

        EXPECT_CALL(*test->mRenderEngine, isCurrent()).WillRepeatedly(Return(true));
    }
};

struct PoweredOffDisplaySetupVariant : public BaseDisplayVariant<PoweredOffDisplaySetupVariant> {
    static constexpr int INIT_POWER_MODE = HWC_POWER_MODE_OFF;

    template <typename Case>
    static void setupCommonCompositionCallExpectations(CompositionTest* test) {
        // TODO: This seems like an unnecessary call if display is powered off.
        EXPECT_CALL(*test->mComposer,
                    setColorTransform(HWC_DISPLAY, _, Hwc2::ColorTransform::IDENTITY))
                .Times(1);

        // TODO: This seems like an unnecessary call if display is powered off.
        Case::CompositionType::setupHwcSetCallExpectations(test);
    }

    static void setupHwcCompositionCallExpectations(CompositionTest*) {}

    static void setupRECompositionCallExpectations(CompositionTest* test) {
        // TODO: This seems like an unnecessary call if display is powered off.
        EXPECT_CALL(*test->mDisplaySurface, getClientTargetAcquireFence())
                .WillRepeatedly(ReturnRef(test->mClientTargetAcquireFence));
    }

    template <typename Case>
    static void setupRELayerCompositionCallExpectations(CompositionTest*) {}
};

/* ------------------------------------------------------------------------
 * Variants for each layer configuration which can be tested
 */

template <typename LayerProperties>
struct BaseLayerProperties {
    static constexpr uint32_t WIDTH = 100;
    static constexpr uint32_t HEIGHT = 100;
    static constexpr PixelFormat FORMAT = PIXEL_FORMAT_RGBA_8888;
    static constexpr uint64_t USAGE =
            GraphicBuffer::USAGE_SW_READ_NEVER | GraphicBuffer::USAGE_SW_WRITE_NEVER;
    static constexpr android_dataspace DATASPACE = HAL_DATASPACE_UNKNOWN;
    static constexpr uint32_t SCALING_MODE = 0;
    static constexpr uint32_t TRANSFORM = 0;
    static constexpr uint32_t LAYER_FLAGS = 0;
    static constexpr float COLOR[] = {1.f, 1.f, 1.f, 1.f};
    static constexpr IComposerClient::BlendMode BLENDMODE =
            IComposerClient::BlendMode::PREMULTIPLIED;

    static void enqueueBuffer(CompositionTest*, sp<BufferQueueLayer> layer) {
        auto producer = layer->getProducer();

        IGraphicBufferProducer::QueueBufferOutput qbo;
        status_t result = producer->connect(nullptr, NATIVE_WINDOW_API_EGL, false, &qbo);
        if (result != NO_ERROR) {
            ALOGE("Failed to connect() (%d)", result);
            return;
        }

        int slot;
        sp<Fence> fence;
        result = producer->dequeueBuffer(&slot, &fence, LayerProperties::WIDTH,
                                         LayerProperties::HEIGHT, LayerProperties::FORMAT,
                                         LayerProperties::USAGE, nullptr, nullptr);
        if (result != IGraphicBufferProducer::BUFFER_NEEDS_REALLOCATION) {
            ALOGE("Failed to dequeueBuffer() (%d)", result);
            return;
        }

        sp<GraphicBuffer> buffer;
        result = producer->requestBuffer(slot, &buffer);
        if (result != NO_ERROR) {
            ALOGE("Failed to requestBuffer() (%d)", result);
            return;
        }

        IGraphicBufferProducer::QueueBufferInput qbi(systemTime(), false /* isAutoTimestamp */,
                                                     LayerProperties::DATASPACE,
                                                     Rect(LayerProperties::WIDTH,
                                                          LayerProperties::HEIGHT),
                                                     LayerProperties::SCALING_MODE,
                                                     LayerProperties::TRANSFORM, Fence::NO_FENCE);
        result = producer->queueBuffer(slot, qbi, &qbo);
        if (result != NO_ERROR) {
            ALOGE("Failed to queueBuffer (%d)", result);
            return;
        }
    }

    static void setupLatchedBuffer(CompositionTest* test, sp<BufferQueueLayer> layer) {
        // TODO: Eliminate the complexity of actually creating a buffer
        EXPECT_CALL(*test->mRenderEngine, getMaxTextureSize()).WillOnce(Return(16384));
        EXPECT_CALL(*test->mRenderEngine, getMaxViewportDims()).WillOnce(Return(16384));
        status_t err =
                layer->setDefaultBufferProperties(LayerProperties::WIDTH, LayerProperties::HEIGHT,
                                                  LayerProperties::FORMAT);
        ASSERT_EQ(NO_ERROR, err);
        Mock::VerifyAndClear(test->mRenderEngine);

        EXPECT_CALL(*test->mMessageQueue, invalidate()).Times(1);
        enqueueBuffer(test, layer);
        Mock::VerifyAndClear(test->mMessageQueue);

        EXPECT_CALL(*test->mRenderEngine, isCurrent()).WillRepeatedly(Return(true));
        EXPECT_CALL(*test->mRenderEngine, useNativeFenceSync()).WillRepeatedly(Return(true));
        EXPECT_CALL(*test->mRenderEngine, createImage())
                .WillOnce(Return(ByMove(std::unique_ptr<renderengine::Image>(test->mReImage))));
        EXPECT_CALL(*test->mRenderEngine, bindExternalTextureImage(DEFAULT_TEXTURE_ID, _)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, checkErrors()).Times(1);
        EXPECT_CALL(*test->mReImage, setNativeWindowBuffer(_, false)).WillOnce(Return(true));
        bool ignoredRecomputeVisibleRegions;
        layer->latchBuffer(ignoredRecomputeVisibleRegions, 0);
        Mock::VerifyAndClear(test->mRenderEngine);
        Mock::VerifyAndClear(test->mReImage);
    }

    static void setupLayerState(CompositionTest* test, sp<BufferQueueLayer> layer) {
        setupLatchedBuffer(test, layer);
    }

    static void setupBufferLayerPostFrameCallExpectations(CompositionTest* test) {
        // BufferLayer::onPostComposition(), when there is no present fence
        EXPECT_CALL(*test->mComposer, getActiveConfig(HWC_DISPLAY, _))
                .WillOnce(DoAll(SetArgPointee<1>(DEFAULT_CONFIG_ID), Return(Error::NONE)));
    }

    static void setupHwcSetGeometryCallExpectations(CompositionTest* test) {
        // TODO: Coverage of other values
        EXPECT_CALL(*test->mComposer,
                    setLayerBlendMode(HWC_DISPLAY, HWC_LAYER, LayerProperties::BLENDMODE))
                .Times(1);
        // TODO: Coverage of other values for origin
        EXPECT_CALL(*test->mComposer,
                    setLayerDisplayFrame(HWC_DISPLAY, HWC_LAYER,
                                         IComposerClient::Rect({0, 0, LayerProperties::WIDTH,
                                                                LayerProperties::HEIGHT})))
                .Times(1);
        EXPECT_CALL(*test->mComposer,
                    setLayerPlaneAlpha(HWC_DISPLAY, HWC_LAYER, LayerProperties::COLOR[3]))
                .Times(1);
        // TODO: Coverage of other values
        EXPECT_CALL(*test->mComposer, setLayerZOrder(HWC_DISPLAY, HWC_LAYER, 0u)).Times(1);
        // TODO: Coverage of other values
        EXPECT_CALL(*test->mComposer, setLayerInfo(HWC_DISPLAY, HWC_LAYER, 0u, 0u)).Times(1);

        // These expectations retire on saturation as the code path these
        // expectations are for appears to make an extra call to them.
        // TODO: Investigate this extra call
        EXPECT_CALL(*test->mComposer, setLayerTransform(HWC_DISPLAY, HWC_LAYER, DEFAULT_TRANSFORM))
                .Times(1)
                .RetiresOnSaturation();
    }

    static void setupHwcSetSourceCropBufferCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerSourceCrop(HWC_DISPLAY, HWC_LAYER,
                                       IComposerClient::FRect({0.f, 0.f, LayerProperties::WIDTH,
                                                               LayerProperties::HEIGHT})))
                .Times(1);
    }

    static void setupHwcSetSourceCropColorCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerSourceCrop(HWC_DISPLAY, HWC_LAYER,
                                       IComposerClient::FRect({0.f, 0.f, 0.f, 0.f})))
                .Times(1);
    }

    static void setupHwcSetPerFrameCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerVisibleRegion(HWC_DISPLAY, HWC_LAYER,
                                          std::vector<IComposerClient::Rect>({IComposerClient::Rect(
                                                  {0, 0, LayerProperties::WIDTH,
                                                   LayerProperties::HEIGHT})})))
                .Times(1);
    }

    static void setupHwcSetPerFrameColorCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, setLayerSurfaceDamage(HWC_DISPLAY, HWC_LAYER, _)).Times(1);

        // TODO: use COLOR
        EXPECT_CALL(*test->mComposer,
                    setLayerColor(HWC_DISPLAY, HWC_LAYER,
                                  IComposerClient::Color({0xff, 0xff, 0xff, 0xff})))
                .Times(1);

        // TODO: ColorLayer::onPreComposition() always returns true, triggering an
        // extra layer update in SurfaceFlinger::preComposition(). This seems
        // wrong on the surface.
        EXPECT_CALL(*test->mMessageQueue, invalidate()).Times(1);
    }

    static void setupHwcSetPerFrameBufferCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, setLayerSurfaceDamage(HWC_DISPLAY, HWC_LAYER, _)).Times(1);
        EXPECT_CALL(*test->mComposer, setLayerBuffer(HWC_DISPLAY, HWC_LAYER, _, _, _)).Times(1);

        setupBufferLayerPostFrameCallExpectations(test);
    }

    static void setupREBufferCompositionCommonCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mRenderEngine,
                    setupLayerBlending(true, false, false,
                                       half4(LayerProperties::COLOR[0], LayerProperties::COLOR[1],
                                             LayerProperties::COLOR[2], LayerProperties::COLOR[3])))
                .Times(1);

        EXPECT_CALL(*test->mRenderEngine, bindExternalTextureImage(DEFAULT_TEXTURE_ID, _)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setupLayerTexturing(_)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setSourceDataSpace(ui::Dataspace::UNKNOWN)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, drawMesh(_)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, disableBlending()).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setSourceY410BT2020(false)).Times(1);
        // This call retires on saturation as the code that renders a texture disables the state,
        // along with a top-level disable to ensure it is disabled for non-buffer layers.
        EXPECT_CALL(*test->mRenderEngine, disableTexturing()).Times(1).RetiresOnSaturation();
    }

    static void setupREBufferCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREBufferCompositionCommonCallExpectations(test);

        // TODO - Investigate and eliminate these differences between display
        // composition and screenshot composition.
        EXPECT_CALL(*test->mRenderEngine, disableScissor()).Times(1);
    }

    static void setupInsecureREBufferCompositionCallExpectations(CompositionTest* test) {
        setupREBufferCompositionCallExpectations(test);
    }

    static void setupREBufferScreenshotCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREBufferCompositionCommonCallExpectations(test);
    }

    static void setupInsecureREBufferScreenshotCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREBufferCompositionCommonCallExpectations(test);
    }

    static void setupREColorCompositionCommonCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mRenderEngine, disableScissor()).Times(1);
    }

    static void setupREColorCompositionCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mRenderEngine, setSourceDataSpace(ui::Dataspace::UNKNOWN)).Times(1);
        EXPECT_CALL(*test->mRenderEngine,
                    setupLayerBlending(true, false, true,
                                       half4(LayerProperties::COLOR[0], LayerProperties::COLOR[1],
                                             LayerProperties::COLOR[2], LayerProperties::COLOR[3])))
                .Times(1);
        EXPECT_CALL(*test->mRenderEngine, drawMesh(_)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, disableBlending()).Times(1);
    }

    static void setupREColorScreenshotCompositionCallExpectations(CompositionTest* test) {
        setupREColorCompositionCallExpectations(test);
    }
};

struct DefaultLayerProperties : public BaseLayerProperties<DefaultLayerProperties> {};

struct ColorLayerProperties : public BaseLayerProperties<ColorLayerProperties> {};

struct SidebandLayerProperties : public BaseLayerProperties<SidebandLayerProperties> {
    using Base = BaseLayerProperties<SidebandLayerProperties>;
    static constexpr IComposerClient::BlendMode BLENDMODE = IComposerClient::BlendMode::NONE;

    static void setupLayerState(CompositionTest* test, sp<BufferQueueLayer> layer) {
        sp<NativeHandle> stream =
                NativeHandle::create(reinterpret_cast<native_handle_t*>(DEFAULT_SIDEBAND_STREAM),
                                     false);
        test->mFlinger.setLayerSidebandStream(layer, stream);
    }

    static void setupHwcSetSourceCropBufferCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerSourceCrop(HWC_DISPLAY, HWC_LAYER,
                                       IComposerClient::FRect({0.f, 0.f, -1.f, -1.f})))
                .Times(1);
    }

    static void setupHwcSetPerFrameBufferCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerSidebandStream(HWC_DISPLAY, HWC_LAYER,
                                           reinterpret_cast<native_handle_t*>(
                                                   DEFAULT_SIDEBAND_STREAM)))
                .WillOnce(Return(Error::NONE));

        EXPECT_CALL(*test->mComposer, setLayerSurfaceDamage(HWC_DISPLAY, HWC_LAYER, _)).Times(1);
    }

    static void setupREBufferCompositionCommonCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mRenderEngine, setupFillWithColor(0, 0, 0, 1)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, drawMesh(_)).Times(1);
    }
};

struct SecureLayerProperties : public BaseLayerProperties<SecureLayerProperties> {
    using Base = BaseLayerProperties<SecureLayerProperties>;

    static constexpr uint32_t LAYER_FLAGS = ISurfaceComposerClient::eSecure;

    static void setupInsecureREBufferCompositionCommonCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mRenderEngine, bindExternalTextureImage(DEFAULT_TEXTURE_ID, _)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setupLayerBlackedOut()).Times(1);

        EXPECT_CALL(*test->mRenderEngine,
                    setupLayerBlending(true, false, false,
                                       half4(Base::COLOR[0], Base::COLOR[1], Base::COLOR[2],
                                             Base::COLOR[3])))
                .Times(1);
        EXPECT_CALL(*test->mRenderEngine, setSourceDataSpace(ui::Dataspace::UNKNOWN)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, drawMesh(_)).Times(1);
        EXPECT_CALL(*test->mRenderEngine, disableBlending()).Times(1);
        EXPECT_CALL(*test->mRenderEngine, setSourceY410BT2020(false)).Times(1);
        // This call retires on saturation as the code that renders a texture disables the state,
        // along with a top-level disable to ensure it is disabled for non-buffer layers.
        EXPECT_CALL(*test->mRenderEngine, disableTexturing()).Times(1).RetiresOnSaturation();
    }

    static void setupInsecureREBufferCompositionCallExpectations(CompositionTest* test) {
        setupInsecureREBufferCompositionCommonCallExpectations(test);
        Base::setupBufferLayerPostFrameCallExpectations(test);
    }

    static void setupInsecureREBufferScreenshotCompositionCallExpectations(CompositionTest* test) {
        setupInsecureREBufferCompositionCommonCallExpectations(test);
    }
};

struct CursorLayerProperties : public BaseLayerProperties<CursorLayerProperties> {
    using Base = BaseLayerProperties<CursorLayerProperties>;

    static void setupLayerState(CompositionTest* test, sp<BufferQueueLayer> layer) {
        Base::setupLayerState(test, layer);
        test->mFlinger.setLayerPotentialCursor(layer, true);
    }
};

struct NoLayerVariant {
    using FlingerLayerType = sp<BufferQueueLayer>;

    static FlingerLayerType createLayer(CompositionTest*) { return FlingerLayerType(); }
    static void injectLayer(CompositionTest*, FlingerLayerType) {}
    static void cleanupInjectedLayers(CompositionTest*) {}

    static void setupCallExpectationsForDirtyGeometry(CompositionTest*) {}
    static void setupCallExpectationsForDirtyFrame(CompositionTest*) {}
};

template <typename LayerProperties>
struct BaseLayerVariant {
    template <typename L, typename F>
    static sp<L> createLayerWithFactory(CompositionTest* test, F factory) {
        EXPECT_CALL(*test->mMessageQueue, postMessage(_, 0)).Times(0);

        sp<L> layer = factory();

        Mock::VerifyAndClear(test->mComposer);
        Mock::VerifyAndClear(test->mRenderEngine);
        Mock::VerifyAndClear(test->mMessageQueue);

        auto& layerDrawingState = test->mFlinger.mutableLayerDrawingState(layer);
        layerDrawingState.layerStack = DEFAULT_LAYER_STACK;
        layerDrawingState.active.w = 100;
        layerDrawingState.active.h = 100;
        layerDrawingState.color = half4(LayerProperties::COLOR[0], LayerProperties::COLOR[1],
                                        LayerProperties::COLOR[2], LayerProperties::COLOR[3]);

        layer->setVisibleRegion(Region(Rect(0, 0, 100, 100)));

        return layer;
    }

    static void injectLayer(CompositionTest* test, sp<Layer> layer) {
        EXPECT_CALL(*test->mComposer, createLayer(HWC_DISPLAY, _))
                .WillOnce(DoAll(SetArgPointee<1>(HWC_LAYER), Return(Error::NONE)));

        layer->createHwcLayer(test->mFlinger.mFlinger->getBE().mHwc.get(), test->mDisplay->getId());

        Mock::VerifyAndClear(test->mComposer);

        Vector<sp<Layer>> layers;
        layers.add(layer);
        test->mDisplay->setVisibleLayersSortedByZ(layers);
        test->mFlinger.mutableDrawingState().layersSortedByZ.add(layer);
    }

    static void cleanupInjectedLayers(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, destroyLayer(HWC_DISPLAY, HWC_LAYER))
                .WillOnce(Return(Error::NONE));
        for (auto layer : test->mFlinger.mutableDrawingState().layersSortedByZ) {
            layer->destroyHwcLayer(test->mDisplay->getId());
        }
        test->mFlinger.mutableDrawingState().layersSortedByZ.clear();
    }
};

template <typename LayerProperties>
struct ColorLayerVariant : public BaseLayerVariant<LayerProperties> {
    using Base = BaseLayerVariant<LayerProperties>;
    using FlingerLayerType = sp<ColorLayer>;

    static FlingerLayerType createLayer(CompositionTest* test) {
        FlingerLayerType layer = Base::template createLayerWithFactory<ColorLayer>(test, [test]() {
            return new ColorLayer(test->mFlinger.mFlinger.get(), sp<Client>(),
                                  String8("test-layer"), LayerProperties::WIDTH,
                                  LayerProperties::HEIGHT, LayerProperties::LAYER_FLAGS);
        });
        return layer;
    }

    static void setupRECompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREColorCompositionCommonCallExpectations(test);
        LayerProperties::setupREColorCompositionCallExpectations(test);
    }

    static void setupREScreenshotCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREColorScreenshotCompositionCallExpectations(test);
    }

    static void setupCallExpectationsForDirtyGeometry(CompositionTest* test) {
        LayerProperties::setupHwcSetGeometryCallExpectations(test);
        LayerProperties::setupHwcSetSourceCropColorCallExpectations(test);
    }

    static void setupCallExpectationsForDirtyFrame(CompositionTest* test) {
        LayerProperties::setupHwcSetPerFrameCallExpectations(test);
        LayerProperties::setupHwcSetPerFrameColorCallExpectations(test);
    }
};

template <typename LayerProperties>
struct BufferLayerVariant : public BaseLayerVariant<LayerProperties> {
    using Base = BaseLayerVariant<LayerProperties>;
    using FlingerLayerType = sp<BufferQueueLayer>;

    static FlingerLayerType createLayer(CompositionTest* test) {
        test->mFlinger.mutableTexturePool().push_back(DEFAULT_TEXTURE_ID);

        FlingerLayerType layer =
                Base::template createLayerWithFactory<BufferQueueLayer>(test, [test]() {
                    return new BufferQueueLayer(test->mFlinger.mFlinger.get(), sp<Client>(),
                                                String8("test-layer"), LayerProperties::WIDTH,
                                                LayerProperties::HEIGHT,
                                                LayerProperties::LAYER_FLAGS);
                });

        LayerProperties::setupLayerState(test, layer);

        return layer;
    }

    static void cleanupInjectedLayers(CompositionTest* test) {
        EXPECT_CALL(*test->mMessageQueue, postMessage(_, 0)).Times(2);
        Base::cleanupInjectedLayers(test);
    }

    static void setupCallExpectationsForDirtyGeometry(CompositionTest* test) {
        LayerProperties::setupHwcSetGeometryCallExpectations(test);
        LayerProperties::setupHwcSetSourceCropBufferCallExpectations(test);
    }

    static void setupCallExpectationsForDirtyFrame(CompositionTest* test) {
        LayerProperties::setupHwcSetPerFrameCallExpectations(test);
        LayerProperties::setupHwcSetPerFrameBufferCallExpectations(test);
    }

    static void setupRECompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREBufferCompositionCallExpectations(test);
    }

    static void setupInsecureRECompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupInsecureREBufferCompositionCallExpectations(test);
    }

    static void setupREScreenshotCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupREBufferScreenshotCompositionCallExpectations(test);
    }

    static void setupInsecureREScreenshotCompositionCallExpectations(CompositionTest* test) {
        LayerProperties::setupInsecureREBufferScreenshotCompositionCallExpectations(test);
    }
};

/* ------------------------------------------------------------------------
 * Variants to control how the composition type is changed
 */

struct NoCompositionTypeVariant {
    static void setupHwcSetCallExpectations(CompositionTest*) {}

    static void setupHwcGetCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, getChangedCompositionTypes(HWC_DISPLAY, _, _)).Times(1);
    }
};

template <IComposerClient::Composition CompositionType>
struct KeepCompositionTypeVariant {
    static constexpr HWC2::Composition TYPE = static_cast<HWC2::Composition>(CompositionType);

    static void setupHwcSetCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerCompositionType(HWC_DISPLAY, HWC_LAYER, CompositionType))
                .Times(1);
    }

    static void setupHwcGetCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, getChangedCompositionTypes(HWC_DISPLAY, _, _)).Times(1);
    }
};

template <IComposerClient::Composition InitialCompositionType,
          IComposerClient::Composition FinalCompositionType>
struct ChangeCompositionTypeVariant {
    static constexpr HWC2::Composition TYPE = static_cast<HWC2::Composition>(FinalCompositionType);

    static void setupHwcSetCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer,
                    setLayerCompositionType(HWC_DISPLAY, HWC_LAYER, InitialCompositionType))
                .Times(1);
    }

    static void setupHwcGetCallExpectations(CompositionTest* test) {
        EXPECT_CALL(*test->mComposer, getChangedCompositionTypes(HWC_DISPLAY, _, _))
                .WillOnce(DoAll(SetArgPointee<1>(std::vector<Hwc2::Layer>{
                                        static_cast<Hwc2::Layer>(HWC_LAYER)}),
                                SetArgPointee<2>(std::vector<IComposerClient::Composition>{
                                        FinalCompositionType}),
                                Return(Error::NONE)));
    }
};

/* ------------------------------------------------------------------------
 * Variants to select how the composition is expected to be handled
 */

struct CompositionResultBaseVariant {
    static void setupLayerState(CompositionTest*, sp<Layer>) {}

    template <typename Case>
    static void setupCallExpectationsForDirtyGeometry(CompositionTest* test) {
        Case::Layer::setupCallExpectationsForDirtyGeometry(test);
    }

    template <typename Case>
    static void setupCallExpectationsForDirtyFrame(CompositionTest* test) {
        Case::Layer::setupCallExpectationsForDirtyFrame(test);
    }
};

struct NoCompositionResultVariant : public CompositionResultBaseVariant {
    template <typename Case>
    static void setupCallExpectations(CompositionTest* test) {
        Case::Display::setupEmptyFrameCompositionCallExpectations(test);
        Case::Display::setupHwcCompositionCallExpectations(test);
    }
};

struct HwcCompositionResultVariant : public CompositionResultBaseVariant {
    template <typename Case>
    static void setupCallExpectations(CompositionTest* test) {
        Case::Display::setupNonEmptyFrameCompositionCallExpectations(test);
        Case::Display::setupHwcCompositionCallExpectations(test);
    }
};

struct RECompositionResultVariant : public CompositionResultBaseVariant {
    template <typename Case>
    static void setupCallExpectations(CompositionTest* test) {
        Case::Display::setupNonEmptyFrameCompositionCallExpectations(test);
        Case::Display::setupRECompositionCallExpectations(test);
        Case::Display::template setupRELayerCompositionCallExpectations<Case>(test);
    }
};

struct ForcedClientCompositionResultVariant : public RECompositionResultVariant {
    static void setupLayerState(CompositionTest*, sp<Layer> layer) {
        layer->forceClientComposition(DisplayDevice::DISPLAY_PRIMARY);
    }

    template <typename Case>
    static void setupCallExpectationsForDirtyGeometry(CompositionTest*) {}

    template <typename Case>
    static void setupCallExpectationsForDirtyFrame(CompositionTest*) {}
};

struct EmptyScreenshotResultVariant {
    static void setupLayerState(CompositionTest*, sp<Layer>) {}

    template <typename Case>
    static void setupCallExpectations(CompositionTest*) {}
};

struct REScreenshotResultVariant : public EmptyScreenshotResultVariant {
    using Base = EmptyScreenshotResultVariant;

    template <typename Case>
    static void setupCallExpectations(CompositionTest* test) {
        Base::template setupCallExpectations<Case>(test);
        Case::Display::template setupRELayerScreenshotCompositionCallExpectations<Case>(test);
    }
};

/* ------------------------------------------------------------------------
 * Composition test case, containing all the variants being tested
 */

template <typename DisplayCase, typename LayerCase, typename CompositionTypeCase,
          typename CompositionResultCase>
struct CompositionCase {
    using ThisCase =
            CompositionCase<DisplayCase, LayerCase, CompositionTypeCase, CompositionResultCase>;
    using Display = DisplayCase;
    using Layer = LayerCase;
    using CompositionType = CompositionTypeCase;
    using CompositionResult = CompositionResultCase;

    static void setupCommon(CompositionTest* test) {
        Display::setupPreconditions(test);

        auto layer = Layer::createLayer(test);
        Layer::injectLayer(test, layer);
        CompositionResult::setupLayerState(test, layer);
    }

    static void setupForDirtyGeometry(CompositionTest* test) {
        setupCommon(test);

        Display::template setupCommonCompositionCallExpectations<ThisCase>(test);
        CompositionResult::template setupCallExpectationsForDirtyGeometry<ThisCase>(test);
        CompositionResult::template setupCallExpectationsForDirtyFrame<ThisCase>(test);
        CompositionResult::template setupCallExpectations<ThisCase>(test);
    }

    static void setupForDirtyFrame(CompositionTest* test) {
        setupCommon(test);

        Display::template setupCommonCompositionCallExpectations<ThisCase>(test);
        CompositionResult::template setupCallExpectationsForDirtyFrame<ThisCase>(test);
        CompositionResult::template setupCallExpectations<ThisCase>(test);
    }

    static void setupForScreenCapture(CompositionTest* test) {
        setupCommon(test);

        Display::template setupCommonScreensCaptureCallExpectations<ThisCase>(test);
        CompositionResult::template setupCallExpectations<ThisCase>(test);
    }

    static void cleanup(CompositionTest* test) {
        Layer::cleanupInjectedLayers(test);

        for (auto& hwcDisplay : test->mFlinger.mFakeHwcDisplays) {
            hwcDisplay->mutableLayers().clear();
        }

        test->mDisplay->setVisibleLayersSortedByZ(Vector<sp<android::Layer>>());
    }
};

/* ------------------------------------------------------------------------
 * Composition cases to test
 */

TEST_F(CompositionTest, noLayersDoesMinimalWorkWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, NoLayerVariant, NoCompositionTypeVariant,
                            NoCompositionResultVariant>>();
}

TEST_F(CompositionTest, noLayersDoesMinimalWorkWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, NoLayerVariant, NoCompositionTypeVariant,
                            NoCompositionResultVariant>>();
}

TEST_F(CompositionTest, noLayersDoesMinimalWorkToCaptureScreen) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, NoLayerVariant, NoCompositionTypeVariant,
                            EmptyScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Simple buffer layers
 */

TEST_F(CompositionTest, HWCComposedNormalBufferLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedNormalBufferLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, REComposedNormalBufferLayer) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
                            ChangeCompositionTypeVariant<IComposerClient::Composition::DEVICE,
                                                         IComposerClient::Composition::CLIENT>,
                            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenNormalBufferLayer) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Single-color layers
 */

TEST_F(CompositionTest, HWCComposedColorLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, ColorLayerVariant<ColorLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::SOLID_COLOR>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedColorLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, ColorLayerVariant<ColorLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::SOLID_COLOR>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, REComposedColorLayer) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, ColorLayerVariant<ColorLayerProperties>,
                            ChangeCompositionTypeVariant<IComposerClient::Composition::SOLID_COLOR,
                                                         IComposerClient::Composition::CLIENT>,
                            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenColorLayer) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, ColorLayerVariant<ColorLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Layers with sideband buffers
 */

TEST_F(CompositionTest, HWCComposedSidebandBufferLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SidebandLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::SIDEBAND>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedSidebandBufferLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SidebandLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::SIDEBAND>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, REComposedSidebandBufferLayer) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SidebandLayerProperties>,
                            ChangeCompositionTypeVariant<IComposerClient::Composition::SIDEBAND,
                                                         IComposerClient::Composition::CLIENT>,
                            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenSidebandBufferLayer) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SidebandLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Layers with ISurfaceComposerClient::eSecure, on a secure display
 */

TEST_F(CompositionTest, HWCComposedSecureBufferLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedSecureBufferLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, REComposedSecureBufferLayer) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            ChangeCompositionTypeVariant<IComposerClient::Composition::DEVICE,
                                                         IComposerClient::Composition::CLIENT>,
                            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenSecureBufferLayerOnSecureDisplay) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Layers with ISurfaceComposerClient::eSecure, on a non-secure display
 */

TEST_F(CompositionTest, HWCComposedSecureBufferLayerOnInsecureDisplayWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<InsecureDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::CLIENT>,
                            ForcedClientCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedSecureBufferLayerOnInsecureDisplayWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<InsecureDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::CLIENT>,
                            ForcedClientCompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenSecureBufferLayerOnInsecureDisplay) {
    captureScreenComposition<
            CompositionCase<InsecureDisplaySetupVariant, BufferLayerVariant<SecureLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Cursor layers
 */

TEST_F(CompositionTest, HWCComposedCursorLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<CursorLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::CURSOR>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, HWCComposedCursorLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<CursorLayerProperties>,
                            KeepCompositionTypeVariant<IComposerClient::Composition::CURSOR>,
                            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, REComposedCursorLayer) {
    displayRefreshCompositionDirtyFrame<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<CursorLayerProperties>,
                            ChangeCompositionTypeVariant<IComposerClient::Composition::CURSOR,
                                                         IComposerClient::Composition::CLIENT>,
                            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenCursorLayer) {
    captureScreenComposition<
            CompositionCase<DefaultDisplaySetupVariant, BufferLayerVariant<CursorLayerProperties>,
                            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

/* ------------------------------------------------------------------------
 *  Simple buffer layer on a display which is powered off.
 */

TEST_F(CompositionTest, displayOffHWCComposedNormalBufferLayerWithDirtyGeometry) {
    displayRefreshCompositionDirtyGeometry<CompositionCase<
            PoweredOffDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, displayOffHWCComposedNormalBufferLayerWithDirtyFrame) {
    displayRefreshCompositionDirtyFrame<CompositionCase<
            PoweredOffDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
            KeepCompositionTypeVariant<IComposerClient::Composition::DEVICE>,
            HwcCompositionResultVariant>>();
}

TEST_F(CompositionTest, displayOffREComposedNormalBufferLayer) {
    displayRefreshCompositionDirtyFrame<CompositionCase<
            PoweredOffDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
            ChangeCompositionTypeVariant<IComposerClient::Composition::DEVICE,
                                         IComposerClient::Composition::CLIENT>,
            RECompositionResultVariant>>();
}

TEST_F(CompositionTest, captureScreenNormalBufferLayerOnPoweredOffDisplay) {
    captureScreenComposition<CompositionCase<
            PoweredOffDisplaySetupVariant, BufferLayerVariant<DefaultLayerProperties>,
            NoCompositionTypeVariant, REScreenshotResultVariant>>();
}

} // namespace
} // namespace android