#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <gif_lib.h>

#include "gif_data.h"


/* ============================================================
   Base64 decoder
   ============================================================ */

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';

    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;

    if (c >= '0' && c <= '9')
        return c - '0' + 52;

    if (c == '+')
        return 62;

    if (c == '/')
        return 63;

    return -1;
}


static unsigned char *base64_decode(
    const char *input,
    size_t *output_size
)
{
    size_t len = strlen(input);

    /* Maximum possible decoded size. */
    unsigned char *output = malloc((len / 4) * 3 + 3);

    if (!output)
        return NULL;

    size_t out = 0;
    int accumulator = 0;
    int bits = 0;

    for (size_t i = 0; i < len; ++i)
    {
        int value = base64_value(input[i]);

        if (value < 0)
            continue;

        accumulator = (accumulator << 6) | value;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            output[out++] =
                (unsigned char)((accumulator >> bits) & 0xff);
        }
    }

    *output_size = out;
    return output;
}


/* ============================================================
   Memory reader for giflib
   ============================================================ */

typedef struct
{
    const unsigned char *data;
    size_t size;
    size_t position;
} MemoryReader;


static int gif_read_callback(
    GifFileType *gif,
    GifByteType *buffer,
    int length
)
{
    MemoryReader *reader =
        (MemoryReader *)gif->UserData;

    size_t remaining =
        reader->size - reader->position;

    if ((size_t)length > remaining)
        length = (int)remaining;

    if (length > 0)
    {
        memcpy(
            buffer,
            reader->data + reader->position,
            length
        );

        reader->position += length;
    }

    return length;
}


/* ============================================================
   Get Graphics Control Extension for a frame
   ============================================================ */

typedef struct
{
    int disposal;
    int delay_ms;
    int transparent;
    int transparent_index;
} FrameInfo;


static FrameInfo get_frame_info(
    SavedImage *image
)
{
    FrameInfo info;

    info.disposal = 0;
    info.delay_ms = 100;
    info.transparent = 0;
    info.transparent_index = 0;

    for (int i = 0; i < image->ExtensionBlockCount; ++i)
    {
        ExtensionBlock *block =
            &image->ExtensionBlocks[i];

        if (block->Function ==
            GRAPHICS_EXT_FUNC_CODE)
        {
            if (block->ByteCount >= 4)
            {
                unsigned char packed =
                    block->Bytes[0];

                int delay =
                    block->Bytes[1] |
                    (block->Bytes[2] << 8);

                info.disposal =
                    (packed >> 2) & 7;

                info.transparent =
                    packed & 1;

                info.transparent_index =
                    block->Bytes[3];

                /*
                   GIF delay is in 1/100 second.
                   Zero-delay GIFs are common, so give
                   them a small practical delay.
                */
                info.delay_ms =
                    delay * 10;

                if (info.delay_ms <= 0)
                    info.delay_ms = 10;
            }
        }
    }

    return info;
}


/* ============================================================
   Get the color map for an image
   ============================================================ */

static ColorMapObject *get_color_map(
    GifFileType *gif,
    SavedImage *image
)
{
    if (image->ImageDesc.ColorMap)
        return image->ImageDesc.ColorMap;

    return gif->SColorMap;
}


/* ============================================================
   Draw a GIF frame onto an RGBA canvas
   ============================================================ */

static void draw_frame(
    GifFileType *gif,
    SavedImage *image,
    unsigned char *canvas,
    int canvas_width,
    int canvas_height
)
{
    ColorMapObject *color_map =
        get_color_map(gif, image);

    if (!color_map)
        return;

    int left = image->ImageDesc.Left;
    int top = image->ImageDesc.Top;

    int width = image->ImageDesc.Width;
    int height = image->ImageDesc.Height;

    FrameInfo info =
        get_frame_info(image);

    const unsigned char *pixels =
        image->RasterBits;

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            int dst_x = left + x;
            int dst_y = top + y;

            if (dst_x < 0 ||
                dst_x >= canvas_width ||
                dst_y < 0 ||
                dst_y >= canvas_height)
                continue;

            int index =
                pixels[y * width + x];

            /*
               Transparent GIF pixels leave the
               previous canvas pixel untouched.
            */
            if (info.transparent &&
                index == info.transparent_index)
            {
                continue;
            }

            if (index < 0 ||
                index >= color_map->ColorCount)
                continue;

            GifColorType color =
                color_map->Colors[index];

            unsigned char *pixel =
                &canvas[
                    (dst_y * canvas_width + dst_x) * 4
                ];

            pixel[0] = color.Red;
            pixel[1] = color.Green;
            pixel[2] = color.Blue;
            pixel[3] = 255;
        }
    }
}


/* ============================================================
   Clear a GIF frame's rectangle
   ============================================================ */

static void clear_frame_area(
    unsigned char *canvas,
    int canvas_width,
    int canvas_height,
    SavedImage *image
)
{
    int left = image->ImageDesc.Left;
    int top = image->ImageDesc.Top;

    int width = image->ImageDesc.Width;
    int height = image->ImageDesc.Height;

    for (int y = 0; y < height; ++y)
    {
        int py = top + y;

        if (py < 0 || py >= canvas_height)
            continue;

        for (int x = 0; x < width; ++x)
        {
            int px = left + x;

            if (px < 0 || px >= canvas_width)
                continue;

            unsigned char *pixel =
                &canvas[(py * canvas_width + px) * 4];

            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }
}


/* ============================================================
   Main
   ============================================================ */

int main(void)
{
    int result = 1;

    unsigned char *gif_data = NULL;
    size_t gif_size = 0;

    GifFileType *gif = NULL;

    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;

    unsigned char *canvas = NULL;
    unsigned char *previous_canvas = NULL;

    /* --------------------------------------------------------
       Decode embedded Base64
       -------------------------------------------------------- */

    gif_data =
        base64_decode(gif_base64, &gif_size);

    if (!gif_data)
    {
        fprintf(stderr,
                "Failed to decode embedded GIF.\n");
        goto cleanup;
    }

    /* --------------------------------------------------------
       Open GIF from memory
       -------------------------------------------------------- */

    MemoryReader reader;

    reader.data = gif_data;
    reader.size = gif_size;
    reader.position = 0;

    int gif_error = 0;

#if GIFLIB_MAJOR >= 5
    gif = DGifOpen(
        &reader,
        gif_read_callback,
        &gif_error
    );
#else
    gif = DGifOpen(
        &reader,
        gif_read_callback
    );
#endif

    if (!gif)
    {
        fprintf(stderr,
                "DGifOpen failed.\n");
        goto cleanup;
    }

    if (DGifSlurp(gif) != GIF_OK)
    {
        fprintf(stderr,
                "Failed to decode GIF.\n");
        goto cleanup;
    }

    int width =
        gif->SWidth;

    int height =
        gif->SHeight;

    if (width <= 0 || height <= 0)
    {
        fprintf(stderr,
                "Invalid GIF dimensions.\n");
        goto cleanup;
    }

    printf(
        "GIF: %dx%d, %d frame(s)\n",
        width,
        height,
        gif->ImageCount
    );

    /* --------------------------------------------------------
       SDL initialization
       -------------------------------------------------------- */

    #define SCALE 10

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    
    window = SDL_CreateWindow(
        "Embedded GIF",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width * SCALE,
        height * SCALE,
        SDL_WINDOW_SHOWN
    );

    if (!window)
    {
        fprintf(stderr,
                "SDL_CreateWindow failed: %s\n",
                SDL_GetError());
        goto cleanup;
    }

    SDL_SetWindowResizable(window, SDL_FALSE);


    /*
       Explicitly prevent resizing.
    */
    SDL_SetWindowResizable(
        window,
        SDL_FALSE
    );

    renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED |
        SDL_RENDERER_PRESENTVSYNC
    );

    if (!renderer)
    {
        fprintf(stderr,
                "SDL_CreateRenderer failed: %s\n",
                SDL_GetError());
        goto cleanup;
    }

    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture)
    {
        fprintf(stderr,
                "SDL_CreateTexture failed: %s\n",
                SDL_GetError());
        goto cleanup;
    }

    /* --------------------------------------------------------
       Create the GIF canvas.
       -------------------------------------------------------- */

    size_t canvas_size =
        (size_t)width *
        (size_t)height *
        4;

    canvas = malloc(canvas_size);

    if (!canvas)
    {
        fprintf(stderr,
                "Out of memory.\n");
        goto cleanup;
    }

    memset(canvas, 0, canvas_size);

    /*
       Used for GIF disposal method 3
       ("restore to previous").
    */
    previous_canvas =
        malloc(canvas_size);

    if (!previous_canvas)
    {
        fprintf(stderr,
                "Out of memory.\n");
        goto cleanup;
    }

    /* --------------------------------------------------------
       Animation loop
       -------------------------------------------------------- */

    int running = 1;

    while (running)
    {
        for (int frame_number = 0;
             frame_number < gif->ImageCount;
             ++frame_number)
        {
            SavedImage *frame =
                &gif->SavedImages[frame_number];

            FrameInfo info =
                get_frame_info(frame);

            /*
               Save canvas before drawing when disposal
               method is "restore to previous".
            */
            if (info.disposal == 3)
            {
                memcpy(
                    previous_canvas,
                    canvas,
                    canvas_size
                );
            }

            /*
               Draw this frame onto the canvas.
            */
            draw_frame(
                gif,
                frame,
                canvas,
                width,
                height
            );

            /*
               Upload canvas to SDL.
            */
            SDL_UpdateTexture(
                texture,
                NULL,
                canvas,
                width * 4
            );

            SDL_RenderClear(renderer);

            /*
               Draw at exactly the GIF's native size.
               No scaling.
            */
            SDL_Rect destination;

            destination.x = 0;
            destination.y = 0;
            destination.w = width * SCALE;
            destination.h = height * SCALE;


            SDL_RenderCopy(
                renderer,
                texture,
                NULL,
                &destination
            );

            SDL_RenderPresent(renderer);

            /*
               Wait for the frame's GIF delay while still
               processing window events.
            */
            Uint32 start =
                SDL_GetTicks();

            while (SDL_GetTicks() - start <
                   (Uint32)info.delay_ms)
            {
                SDL_Event event;

                while (SDL_PollEvent(&event))
                {
                    if (event.type == SDL_QUIT)
                        running = 0;

                    /*
                       Since the window is non-resizable,
                       this is mostly defensive.
                    */
                    if (event.type ==
                        SDL_WINDOWEVENT &&
                        event.window.event ==
                        SDL_WINDOWEVENT_CLOSE)
                    {
                        running = 0;
                    }
                }

                if (!running)
                    break;

                SDL_Delay(1);
            }

            if (!running)
                break;

            /*
               Apply GIF disposal method.
            */

            if (info.disposal == 2)
            {
                /*
                   Restore frame area to the GIF background.
                   Black is a safe generic background.
                */
                clear_frame_area(
                    canvas,
                    width,
                    height,
                    frame
                );
            }
            else if (info.disposal == 3)
            {
                /*
                   Restore the canvas from before
                   this frame was drawn.
                */
                memcpy(
                    canvas,
                    previous_canvas,
                    canvas_size
                );
            }
        }
    }

    result = 0;

cleanup:

    if (texture)
        SDL_DestroyTexture(texture);

    if (renderer)
        SDL_DestroyRenderer(renderer);

    if (window)
        SDL_DestroyWindow(window);

    if (gif)
    {
        int close_error = 0;

#if GIFLIB_MAJOR >= 5
        DGifCloseFile(gif, &close_error);
#else
        DGifCloseFile(gif);
#endif
    }

    free(previous_canvas);
    free(canvas);
    free(gif_data);

    SDL_Quit();

    return result;
}
