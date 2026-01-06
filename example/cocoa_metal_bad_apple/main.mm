#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include <utils/log/log.hpp>
#include <platform/schedule/eventloop.h>
#include <platform/macos/schedule/cocoa_backend.h>
#include <memory>
#include <vector>
#include <functional>

// 包含 Bad Apple 数据加载器
#include "bad_apple_data.h"

// 全局实例定义
BadAppleData g_bad_apple_data;

using namespace RopHive;
using namespace RopHive::MacOS;

// 前向声明
class BadApplePlayer;

// 鼠标事件监听器类
class MouseClickWatcher : public RopHive::IWatcher {
public:
    using TogglePauseCallback = std::function<void()>;

    explicit MouseClickWatcher(RopHive::EventLoop& loop, TogglePauseCallback callback)
        : IWatcher(loop), toggle_pause_callback_(std::move(callback)) {}

    void start() override {
        // 创建鼠标点击事件源
        mouse_source_ = std::make_shared<RopHive::MacOS::CocoaEventTypeSource>(
            (int)NSEventTypeLeftMouseDown,
            [this](const RopHive::MacOS::CocoaRawEvent& event) {
                this->onMouseDown(event);
            }
        );
        attachSource(mouse_source_);
    }

    void stop() override {
        if (mouse_source_) {
            detachSource(mouse_source_);
        }
    }

private:
    void onMouseDown(const RopHive::MacOS::CocoaRawEvent& event) {
        NSEvent* nsEvent = static_cast<NSEvent*>(event.event);
        LOG(INFO)("Mouse clicked at (%.1f, %.1f)", [nsEvent locationInWindow].x, [nsEvent locationInWindow].y);
        if (toggle_pause_callback_) {
            toggle_pause_callback_();
        }
    }

    TogglePauseCallback toggle_pause_callback_;
    std::shared_ptr<RopHive::MacOS::CocoaEventTypeSource> mouse_source_;
};

// Metal 渲染器
@interface MetalRenderer : NSObject<MTKViewDelegate>
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pipelineState;
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, assign) NSUInteger frameIndex;
@property (nonatomic, assign) NSTimeInterval lastFrameTime;

- (instancetype)initWithView:(MTKView*)view;
- (void)updateFrame;
@end

@implementation MetalRenderer

- (instancetype)initWithView:(MTKView*)view {
    self = [super init];
    if (self) {
        _device = view.device;
        _commandQueue = [_device newCommandQueue];
        _frameIndex = 0;
        _lastFrameTime = 0;

        [self setupPipeline:view];
        [self createTexture];
    }
    return self;
}

- (void)setupPipeline:(MTKView*)view {
    // 创建渲染管道
    NSString* shaderSource = @
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "\n"
        "struct VertexOut {\n"
        "    float4 position [[position]];\n"
        "    float2 texCoord;\n"
        "};\n"
        "\n"
        "vertex VertexOut vertexShader(uint vertexID [[vertex_id]]) {\n"
        "    float2 positions[4] = {\n"
        "        float2(-1.0, -1.0),\n"
        "        float2( 1.0, -1.0),\n"
        "        float2(-1.0,  1.0),\n"
        "        float2( 1.0,  1.0)\n"
        "    };\n"
        "    float2 texCoords[4] = {\n"
        "        float2(0.0, 1.0),\n"
        "        float2(1.0, 1.0),\n"
        "        float2(0.0, 0.0),\n"
        "        float2(1.0, 0.0)\n"
        "    };\n"
        "    VertexOut out;\n"
        "    out.position = float4(positions[vertexID], 0.0, 1.0);\n"
        "    out.texCoord = texCoords[vertexID];\n"
        "    return out;\n"
        "}\n"
        "\n"
        "fragment float4 fragmentShader(VertexOut in [[stage_in]],\n"
        "                              texture2d<float> tex [[texture(0)]],\n"
        "                              sampler sam [[sampler(0)]]) {\n"
        "    return tex.sample(sam, in.texCoord);\n"
        "}\n";

    NSError* error = nil;
    id<MTLLibrary> library = [_device newLibraryWithSource:shaderSource options:nil error:&error];
    if (!library) {
        LOG(ERROR)("Failed to create Metal library: %s", [[error description] UTF8String]);
        return;
    }

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertexShader"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragmentShader"];

    MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    pipelineDescriptor.vertexFunction = vertexFunction;
    pipelineDescriptor.fragmentFunction = fragmentFunction;
    pipelineDescriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat;

    _pipelineState = [_device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
    if (!_pipelineState) {
        printf("Failed to create render pipeline state: %s\n", [[error description] UTF8String]);
    }
}

- (void)createTexture {
    // 使用数据的尺寸
    MTLTextureDescriptor* textureDescriptor = [[MTLTextureDescriptor alloc] init];
    textureDescriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
    textureDescriptor.width = BAD_APPLE_WIDTH;
    textureDescriptor.height = BAD_APPLE_HEIGHT;
    textureDescriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    textureDescriptor.storageMode = MTLStorageModeManaged; // 支持 CPU 和 GPU 访问

    _texture = [_device newTextureWithDescriptor:textureDescriptor];

    if (!_texture) {
        printf("Failed to create texture with size %dx%d\n", BAD_APPLE_WIDTH, BAD_APPLE_HEIGHT);
    } else {
        printf("Created texture with size %dx%d\n", BAD_APPLE_WIDTH, BAD_APPLE_HEIGHT);
    }
}

- (void)updateFrame {
    // 显示帧更新状态
    if (_frameIndex == 0) {
        LOG(INFO)("Starting frame updates - data loaded: %s", g_bad_apple_data.isLoaded() ? "YES" : "NO");
    }

    // 使用 Bad Apple 帧数据
    int currentFrame = _frameIndex % BAD_APPLE_FRAME_COUNT;
    std::vector<uint8_t> rgbaData = g_bad_apple_data.getFrameRGBA(currentFrame);

    if (!rgbaData.empty()) {
        // 更新纹理
        MTLRegion region = MTLRegionMake2D(0, 0, BAD_APPLE_WIDTH, BAD_APPLE_HEIGHT);
        [_texture replaceRegion:region
                    mipmapLevel:0
                      withBytes:rgbaData.data()
                    bytesPerRow:BAD_APPLE_WIDTH * 4];

        // 显示当前帧号（每10帧显示一次以避免日志过多）
        if (_frameIndex % 10 == 0) {
            LOG(INFO)("Playing frame %d / %d (data size: %lu)", currentFrame + 1, BAD_APPLE_FRAME_COUNT, rgbaData.size());
        }
    } else {
        LOG(WARN)("Failed to get frame data for frame %d", currentFrame);
    }

    _frameIndex++;
}

- (void)drawInMTKView:(MTKView*)view {
    @autoreleasepool {
        id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
        MTLRenderPassDescriptor* renderPassDescriptor = view.currentRenderPassDescriptor;

        if (renderPassDescriptor) {
            id<MTLRenderCommandEncoder> renderEncoder =
                [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

            [renderEncoder setRenderPipelineState:_pipelineState];
            [renderEncoder setFragmentTexture:_texture atIndex:0];

            MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
            samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
            samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
            id<MTLSamplerState> sampler = [_device newSamplerStateWithDescriptor:samplerDescriptor];
            [renderEncoder setFragmentSamplerState:sampler atIndex:0];

            [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                              vertexStart:0
                              vertexCount:4];

            [renderEncoder endEncoding];

            [commandBuffer presentDrawable:view.currentDrawable];
        }

        [commandBuffer commit];
    }
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    // 处理视图大小变化
}

@end

// Bad Apple 播放器类
class BadApplePlayer {
public:
    explicit BadApplePlayer(RopHive::EventLoop& loop)
        : loop_(loop), window_(nil), mtkView_(nil), renderer_(nil), mouse_watcher_(nullptr), isPaused_(false) {}

    ~BadApplePlayer() {
        stop();
    }

    void start() {
        LOG(INFO)("Starting Bad Apple player...");

        [NSApplication sharedApplication];

        // 创建窗口 - 使用视频的宽高比
        const float aspectRatio = (float)BAD_APPLE_WIDTH / BAD_APPLE_HEIGHT;
        const int windowHeight = 600;
        const int windowWidth = (int)(windowHeight * aspectRatio);

        NSRect frame = NSMakeRect(0, 0, windowWidth, windowHeight);
        window_ = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:(NSWindowStyleMaskTitled |
                                                         NSWindowStyleMaskClosable |
                                                         NSWindowStyleMaskResizable)
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];

        [window_ setTitle:@"Bad Apple - Cocoa + Metal"];
        [window_ center];

        // 首先激活应用程序
        [NSApp activateIgnoringOtherApps:YES];
        // 然后显示窗口
        [window_ makeKeyAndOrderFront:nil];

        // 创建 Metal 视图
        mtkView_ = [[MTKView alloc] initWithFrame:frame];
        mtkView_.device = MTLCreateSystemDefaultDevice();
        mtkView_.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        mtkView_.colorPixelFormat = MTLPixelFormatBGRA8Unorm;

        // 创建渲染器
        renderer_ = [[MetalRenderer alloc] initWithView:mtkView_];
        mtkView_.delegate = renderer_;

        [window_ setContentView:mtkView_];

        // 创建鼠标事件监听器
        mouse_watcher_ = new MouseClickWatcher(loop_, [this]() { this->togglePause(); });
        mouse_watcher_->start();

        // 设置窗口关闭回调
        [window_ setReleasedWhenClosed:NO];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                          object:window_
                                                           queue:nil
                                                      usingBlock:^(NSNotification* notification) {
            LOG(INFO)("Window close notification received");
            loop_.requestExit();
        }];

        // 启动帧更新循环 (30 FPS)
        startFrameLoop();

        LOG(INFO)("Bad Apple player started");
    }

    void stop() {
        LOG(INFO)("Stopping Bad Apple player...");

        if (mouse_watcher_) {
            mouse_watcher_->stop();
            delete mouse_watcher_;
            mouse_watcher_ = nullptr;
        }

        if (window_) {
            [[NSNotificationCenter defaultCenter] removeObserver:(id)this];
            [window_ close];
            window_ = nil;
        }

        mtkView_ = nil;
        renderer_ = nil;

        LOG(INFO)("Bad Apple player stopped");
    }

    void togglePause() {
        isPaused_ = !isPaused_;
        LOG(INFO)("Bad Apple player %s", isPaused_ ? "PAUSED" : "RESUMED");
    }

private:
    void startFrameLoop() {
        // 使用原始指针而不是 weak_ptr 来避免 shared_ptr 问题
        loop_.postDelayed([this]() {
            if (!isPaused_) {
                [renderer_ updateFrame];
                [mtkView_ setNeedsDisplay:YES];
            }

            // 在第一帧确保窗口在最前面
            static bool firstFrame = true;
            if (firstFrame) {
                firstFrame = false;
                // 再次确保窗口在最前面
                [window_ makeKeyAndOrderFront:nil];
                [NSApp activateIgnoringOtherApps:YES];
            }

            startFrameLoop(); // 继续下一帧
        }, std::chrono::milliseconds(33)); // ~30 FPS
    }

    RopHive::EventLoop& loop_;
    NSWindow* window_;
    MTKView* mtkView_;
    MetalRenderer* renderer_;
    MouseClickWatcher* mouse_watcher_;
    bool isPaused_;
};



int main(int argc, char* argv[]) {
    LOG(INFO)("Starting Cocoa + Metal Bad Apple Example");

    @autoreleasepool {
        // 初始化 Cocoa 应用程序
        [NSApplication sharedApplication];

        // 激活应用程序，让它获得焦点
        [NSApp activateIgnoringOtherApps:YES];

        try {
            EventLoop loop(BackendType::MACOS_COCOA);
            auto player = std::make_shared<BadApplePlayer>(loop);

            LOG(INFO)("Starting player...");
            player->start();

            LOG(INFO)("Event loop starting - Bad Apple is playing");
            loop.run();

            LOG(INFO)("Event loop ended");
        } catch (const std::exception& e) {
            LOG(ERROR)("Exception caught in main: %s", e.what());
            return 1;
        } catch (...) {
            LOG(ERROR)("Unknown exception caught in main");
            return 1;
        }
    }

    LOG(INFO)("Cocoa + Metal Bad Apple Example finished");
    return 0;
}

#endif
