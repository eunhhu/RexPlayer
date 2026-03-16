#include <gtest/gtest.h>
#include "software_renderer.h"

using namespace rex::gpu;

TEST(SoftwareRenderer, InitializeAndDestroy) {
    SoftwareRenderer renderer;
    auto result = renderer.initialize();
    ASSERT_TRUE(result.has_value());
    renderer.destroy();
}

TEST(SoftwareRenderer, CreateContext) {
    SoftwareRenderer renderer;
    renderer.initialize();

    auto ctx = renderer.create_context();
    ASSERT_TRUE(ctx.has_value());
    EXPECT_GT(*ctx, 0u);

    renderer.destroy_context(*ctx);
}

TEST(SoftwareRenderer, CreateResource) {
    SoftwareRenderer renderer;
    renderer.initialize();

    bool ok = renderer.create_resource(1, 100, 100, PixelFormat::BGRA8888);
    EXPECT_TRUE(ok);

    // Duplicate should fail
    bool dup = renderer.create_resource(1, 200, 200, PixelFormat::RGBA8888);
    EXPECT_FALSE(dup);

    renderer.destroy_resource(1);
}

TEST(SoftwareRenderer, TransferToHost) {
    SoftwareRenderer renderer;
    renderer.initialize();

    renderer.create_resource(1, 4, 4, PixelFormat::BGRA8888);

    // Create a 2x2 red patch at (1,1)
    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    // BGRA: Blue=0, Green=0, Red=255, Alpha=255
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i]     = 0;
        pixels[i + 1] = 0;
        pixels[i + 2] = 255;
        pixels[i + 3] = 255;
    }

    bool ok = renderer.transfer_to_host(1, 1, 1, 2, 2, pixels.data(), pixels.size());
    EXPECT_TRUE(ok);

    // Out of bounds should fail
    bool oob = renderer.transfer_to_host(1, 3, 3, 2, 2, pixels.data(), pixels.size());
    EXPECT_FALSE(oob);
}

TEST(SoftwareRenderer, SetScanoutAndGetFramebuffer) {
    SoftwareRenderer renderer;
    renderer.initialize();

    auto ctx = renderer.create_context();
    ASSERT_TRUE(ctx.has_value());

    renderer.create_resource(1, 64, 64, PixelFormat::BGRA8888);

    // Attach backing
    std::vector<uint8_t> data(64 * 64 * 4, 128);
    renderer.attach_backing(1, data.data(), data.size());

    // Set scanout
    bool ok = renderer.set_scanout(*ctx, 1, 0, 0, 64, 64);
    EXPECT_TRUE(ok);

    // Get framebuffer
    auto fb_result = renderer.get_framebuffer(*ctx);
    ASSERT_TRUE(fb_result.has_value());

    const FrameBuffer* fb = *fb_result;
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->width, 64u);
    EXPECT_EQ(fb->height, 64u);
    EXPECT_EQ(fb->format, PixelFormat::BGRA8888);
    EXPECT_NE(fb->data, nullptr);
}

TEST(SoftwareRenderer, FlushResource) {
    SoftwareRenderer renderer;
    renderer.initialize();

    renderer.create_resource(1, 32, 32, PixelFormat::BGRA8888);

    bool ok = renderer.flush_resource(1, 0, 0, 32, 32);
    EXPECT_TRUE(ok);

    // Non-existent resource
    bool bad = renderer.flush_resource(999, 0, 0, 32, 32);
    EXPECT_FALSE(bad);
}

TEST(SoftwareRenderer, DestroyResourceClearsScanout) {
    SoftwareRenderer renderer;
    renderer.initialize();

    auto ctx = renderer.create_context();
    renderer.create_resource(1, 32, 32, PixelFormat::BGRA8888);
    renderer.set_scanout(*ctx, 1, 0, 0, 32, 32);

    // Destroying resource should clear the scanout
    renderer.destroy_resource(1);

    auto fb = renderer.get_framebuffer(*ctx);
    EXPECT_FALSE(fb.has_value());
}

TEST(SoftwareRenderer, NoFramebufferWithoutScanout) {
    SoftwareRenderer renderer;
    renderer.initialize();

    auto ctx = renderer.create_context();
    auto fb = renderer.get_framebuffer(*ctx);
    EXPECT_FALSE(fb.has_value());
}

TEST(SoftwareRenderer, RendererFactory) {
    auto sw = create_renderer(RendererType::Software);
    EXPECT_EQ(sw->type(), RendererType::Software);
    EXPECT_EQ(sw->name(), "Software");
}

TEST(SoftwareRenderer, PixelFormatBpp) {
    EXPECT_EQ(bytes_per_pixel(PixelFormat::BGRA8888), 4u);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::RGBA8888), 4u);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::RGB565), 2u);
    EXPECT_EQ(bytes_per_pixel(PixelFormat::XRGB8888), 4u);
}
