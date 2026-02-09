#include <SDL3/SDL.h>
#include <stdbool.h>

// Sentinel texture pointer — atlas only stores and checks for non-NULL
static int stub_texture_storage;

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, SDL_PixelFormat format,
                               SDL_TextureAccess access, int w, int h)
{
    (void)renderer;
    (void)format;
    (void)access;
    (void)w;
    (void)h;
    return (SDL_Texture *)&stub_texture_storage;
}

void SDL_DestroyTexture(SDL_Texture *texture)
{
    (void)texture;
}

bool SDL_SetTextureBlendMode(SDL_Texture *texture, SDL_BlendMode blendMode)
{
    (void)texture;
    (void)blendMode;
    return true;
}

bool SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
    (void)texture;
    (void)scaleMode;
    return true;
}

bool SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
                       const void *pixels, int pitch)
{
    (void)texture;
    (void)rect;
    (void)pixels;
    (void)pitch;
    return true;
}

const char *SDL_GetError(void)
{
    return "stub";
}

bool SDL_GetRectUnion(const SDL_Rect *A, const SDL_Rect *B, SDL_Rect *result)
{
    if (!A || !B || !result)
        return false;
    int x1 = A->x < B->x ? A->x : B->x;
    int y1 = A->y < B->y ? A->y : B->y;
    int x2a = A->x + A->w;
    int x2b = B->x + B->w;
    int y2a = A->y + A->h;
    int y2b = B->y + B->h;
    int x2 = x2a > x2b ? x2a : x2b;
    int y2 = y2a > y2b ? y2a : y2b;
    result->x = x1;
    result->y = y1;
    result->w = x2 - x1;
    result->h = y2 - y1;
    return true;
}
