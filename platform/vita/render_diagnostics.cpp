#include "platform/vita/render_diagnostics.hpp"
#ifdef ONS_VITA_RENDER_DIAGNOSTICS
#include "Engine/Core/ONScripter.hpp"
#include "Engine/Components/Dialogue.hpp"
#include "Engine/Components/TextWindow.hpp"

void VitaPlatform::captureRenderDiagnostics() {
    // Run once at a stable title frame. Never read or save live game layers:
    // multi-megabyte filesystem writes stall effects on the Vita's storage.
    static bool probed;
    const char *label = ons.current_label_info ? ons.current_label_info->name : nullptr;
    static unsigned traces, lastTrace;
    if (traces < 16 && dlgCtrl.dialogueProcessingState.active && SDL_GetTicks() - lastTrace >= 1000) {
        lastTrace = SDL_GetTicks();
        ++traces;
        std::fprintf(stdout, "Native dialogue: label=%s display=%d refresh=%d dynamic=%d segment=%d rendering=%d window=%p\n",
            label ? label : "", ons.display_mode, ons.refreshMode(), wndCtrl.usingDynamicTextWindow,
            dlgCtrl.dialogueRenderState.segmentIndex, dlgCtrl.dialogueIsRendering,
            static_cast<void *>(ons.sentence_font_info.gpu_image));
    }
    if (probed || !label || std::strcmp(label, "b_title_button_loop") != 0) return;
    probed = true;
    GPU_FlushBlitBuffer();
    GPU_Context *context = GPU_GetContextTarget()->context;
    const Uint32 program = context->current_shader_program;
    GPU_ShaderBlock block = context->current_shader_block;
    GPU_DeactivateShaderProgram();
    GPU_Image *target = gpu.createTargetImage(128, 128, 4);
    GPU_Image *pixel = gpu.createImage(2, 2, 4, false, false);
    const unsigned char pixels[] = {255,255,255,255, 255,255,255,255,
                                    255,255,255,255, 255,255,255,255};
    GPU_UpdateImageBytes(pixel, nullptr, pixels, 8);
    GPU_Rect clips[] = {{0,0,128,128}, {24,80,48,32}};
    for (unsigned test = 0; test < 4; ++test) {
        gpu.clearWholeTarget(target->target);
        GPU_Rect *clip = test == 1 ? &clips[0] : test == 2 ? &clips[1] : nullptr;
        gpu.copyGPUImage(pixel, nullptr, clip, target->target, 64,64,64,64,0,true);
        if (test == 3) {
            GPU_SetClipRect(target->target, clips[1]);
            gpu.clear(target->target);
            GPU_UnsetClip(target->target);
        }
        GPU_FlushBlitBuffer();
        SDL_Surface *surface = GPU_CopySurfaceFromImage(target);
        unsigned mismatches = 0, alphaPixels = 0;
        if (surface) {
            for (int y=0; y<surface->h; ++y) for (int x=0; x<surface->w; ++x) {
                Uint32 value;
                std::memcpy(&value, static_cast<unsigned char *>(surface->pixels) + y*surface->pitch + x*4, 4);
                Uint8 r,g,b,a;
                SDL_GetRGBA(value, surface->format, &r,&g,&b,&a);
                const bool inside = x>=12 && x<36 && y>=40 && y<56;
                const bool expected = test == 2 ? inside : test == 3 ? !inside : true;
                alphaPixels += a != 0;
                mismatches += expected ? (a!=255 || r!=255 || g!=255 || b!=255) : a!=0;
            }
            std::fprintf(stdout, "Native GPU probe %u: %dx%d mismatches=%u alpha-pixels=%u expected=%u\n",
                test, surface->w, surface->h, mismatches, alphaPixels, test==2 ? 384u : test==3 ? 3712u : 4096u);
            SDL_FreeSurface(surface);
        } else std::fprintf(stderr, "Native GPU probe %u: readback failed\n", test);
    }
    GPU_Image *odd = gpu.createTargetImage(126,122,4);
    for (unsigned test=0; test<2; ++test) {
        gpu.clearWholeTarget(odd->target);
        gpu.copyGPUImage(pixel,nullptr,test?&clips[1]:nullptr,odd->target,63,61,63,61,0,true);
        GPU_FlushBlitBuffer();
        SDL_Surface *surface=GPU_CopySurfaceFromImage(odd);
        unsigned mismatches=0;
        if (surface) {
            for (int y=0;y<surface->h;++y) for (int x=0;x<surface->w;++x) {
                Uint32 value;
                std::memcpy(&value,static_cast<unsigned char *>(surface->pixels)+y*surface->pitch+x*4,4);
                Uint8 r,g,b,a; SDL_GetRGBA(value,surface->format,&r,&g,&b,&a);
                bool expected=!test || (x>=12 && x<36 && y>=40 && y<56);
                mismatches+=expected?(a!=255||r!=255||g!=255||b!=255):a!=0;
            }
            std::fprintf(stdout,"Native odd GPU probe %u: %dx%d mismatches=%u\n",test,surface->w,surface->h,mismatches);
            SDL_FreeSurface(surface);
        } else std::fprintf(stderr,"Native odd GPU probe %u: readback failed\n",test);
    }
    gpu.freeImage(odd);
    gpu.freeImage(pixel);
    // Exercise the game's actual font/cache/atlas path, not just solid quads.
    // Read back only this 64x64 target, never the full atlas or game layers.
    gpu.clearWholeTarget(target->target);
    Fontinfo font = ons.sentence_font;
    font.changeStyle().color = {255,255,255};
    const GlyphValues *glyph = font.renderUnicodeGlyph('A');
    TextRenderingState::TextRenderingDst destination{};
    destination.target = target->target;
    ons.renderGlyphValues(*glyph, nullptr, destination, 16,16,1,false,255);
    GPU_FlushBlitBuffer();
    SDL_Surface *ink = GPU_CopySurfaceFromImage(target);
    unsigned coverage = 0, coloured = 0;
    if (ink) {
        for (int y=0; y<ink->h; ++y) for (int x=0; x<ink->w; ++x) {
            Uint32 value;
            std::memcpy(&value, static_cast<unsigned char *>(ink->pixels)+y*ink->pitch+x*4, 4);
            Uint8 r,g,b,a; SDL_GetRGBA(value, ink->format, &r,&g,&b,&a);
            coverage += a!=0; coloured += a!=0 && (r||g||b);
        }
        SDL_FreeSurface(ink);
    }
    std::fprintf(stdout, "Native glyph probe: coverage=%u coloured=%u result=%s\n",
        coverage, coloured, coverage && coloured ? "PASS" : "FAIL");
    gpu.freeImage(target);
    GPU_ActivateShaderProgram(program, &block);
}
#else
void VitaPlatform::captureRenderDiagnostics() {}
#endif
