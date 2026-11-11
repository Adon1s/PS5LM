// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// PS5LM Lean: one image at the image-token cap, sent by the app to its own
// server once the model is ready, so the vision encoder's GPU buffers are
// sized once, at their largest, before photos arrive.
//
// llama.cpp warms the encoder at load with a square image of at most the cap
// (35 x 35 = 1,225 tokens under --image-max-tokens 1280) and ggml's Vulkan
// backend sizes its working buffers at the first real encode. Each photo with
// more tokens than any before rebuilt them bigger: the new buffer was cut from
// the largest free block and the old one left a hole elsewhere. Over 25 photos
// that took 578 MiB from the largest block (IQ2_S, 2026-11-11); after one image
// at the cap first, 356 MiB, and none after it (measured from the PC).
//
// Only with --mmproj and --image-max-tokens N in the server's arguments: the
// image is 32 pixels a token side (Qwen-VL's 16-pixel patches merged 2 x 2),
// the most tokens N allows, no longer than twice its height. Off with "off" on
// the first line of /data/PS5LM/warmup.txt.

#include <string>
#include <vector>

namespace ps5lm {

// Starts the warm-up on a thread of its own when the arguments call for it and
// warmup.txt does not say off. False when there is nothing to do.
bool vision_warmup_start(int port, const std::vector<std::string> & server_args, const std::string & warmup_txt);

// Once, when the warm-up has finished: true, with what to log.
bool vision_warmup_finished(std::string & summary);

// The image's size for a cap of n_tokens, 32 pixels a token side (exposed for a host test).
void vision_warmup_size(int n_tokens, int & width, int & height);

}  // namespace ps5lm
