// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace hui::gfx {
class Renderer;
}

namespace ps5lm {

// The TV, through ps5-opengl: one OpenGL 4.6 context, presented at vsync.
class Display {
  public:
    // PS5LM Lean: without a depth or stencil buffer unless `depth` (the
    // dashboard uses neither); a failed open leaves nothing open.
    bool open(int width, int height, bool depth = false);
    void clear(float r, float g, float b);
    // Draws the renderer's frame into the TV's framebuffer.
    void present(hui::gfx::Renderer & renderer);
    bool swap();
    // PS5LM Lean: ends the context, the surface and the display; eglTerminate
    // hands the GL port's memory back. open() works again after it (every GL
    // object is gone: the renderer and the font textures are made again).
    void close();
    int  width() const { return width_; }
    int  height() const { return height_; }

  private:
    int width_  = 0;
    int height_ = 0;
};

}  // namespace ps5lm
