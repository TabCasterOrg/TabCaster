#include "png_encoder.h"
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t size;
    size_t allocated;
    size_t offset;
} PNGMemoryData;

// PNG write callback for memory
static void png_write_data_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    PNGMemoryData *mem_data = (PNGMemoryData*)png_get_io_ptr(png_ptr);
    
    // Ensure we have enough space
    if (mem_data->offset + length > mem_data->allocated) {
        size_t new_size = mem_data->allocated * 2;
        if (new_size < mem_data->offset + length) {
            new_size = mem_data->offset + length + 4096;
        }
        
        unsigned char *new_data = realloc(mem_data->data, new_size);
        if (!new_data) {
            png_error(png_ptr, "Failed to allocate memory for PNG");
            return;
        }
        
        mem_data->data = new_data;
        mem_data->allocated = new_size;
    }
    
    memcpy(mem_data->data + mem_data->offset, data, length);
    mem_data->offset += length;
    mem_data->size = mem_data->offset;
}

static void png_flush_callback(png_structp png_ptr) {
    // No-op for memory writing
    (void)png_ptr;
}

// Encode raw RGB24 buffer to PNG 
int png_encode_rgb(const unsigned char *rgb,
                   int width,
                   int height,
                   unsigned char **png_data,
                   size_t *png_size) {
    if (!rgb || width <= 0 || height <= 0 || !png_data || !png_size) return -1;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return -1;

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        return -1;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return -1;
    }

    PNGMemoryData mem_data = (PNGMemoryData){0};
    mem_data.allocated = (size_t)width * (size_t)height * 2;
    mem_data.data = malloc(mem_data.allocated);
    if (!mem_data.data) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        return -1;
    }

    png_set_write_fn(png_ptr, &mem_data, png_write_data_callback, png_flush_callback);
    png_set_IHDR(png_ptr, info_ptr, width, height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png_ptr, 1);
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
    png_write_info(png_ptr, info_ptr);

    png_bytep *row_pointers = malloc(sizeof(png_bytep) * (size_t)height);
    if (!row_pointers) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(mem_data.data);
        return -1;
    }
    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep)(rgb + (size_t)y * (size_t)width * 3);
    }
    png_write_image(png_ptr, row_pointers);
    png_write_end(png_ptr, NULL);
    free(row_pointers);
    png_destroy_write_struct(&png_ptr, &info_ptr);

    *png_data = mem_data.data;
    *png_size = mem_data.size;
    return 0;
}

// Helper function to convert full frame from BGRX to RGB
void png_convert_bgrx_to_rgb(const XImage *frame, unsigned char *rgb_out) {
    if (!frame || !rgb_out) return;
    
    unsigned char *src = (unsigned char*)frame->data;
    for (int y = 0; y < frame->height; y++) {
        unsigned char *line_src = src + (y * frame->bytes_per_line);
        unsigned char *line_dst = rgb_out + (size_t)y * (size_t)frame->width * 3;
        for (int x = 0; x < frame->width; x++) {
            unsigned char b = line_src[x * 4 + 0];
            unsigned char g = line_src[x * 4 + 1];
            unsigned char r = line_src[x * 4 + 2];
            line_dst[x * 3 + 0] = r;
            line_dst[x * 3 + 1] = g;
            line_dst[x * 3 + 2] = b;
        }
    }
}

