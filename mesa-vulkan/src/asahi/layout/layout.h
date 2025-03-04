/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 *
 */

#pragma once

#include "drm-uapi/drm_fourcc.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/u_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIL_CACHELINE      0x80
#define AIL_PAGESIZE       0x4000
#define AIL_MAX_MIP_LEVELS 16

enum ail_tiling {
   /**
    * Strided linear (raster order). Only allowed for 1D or 2D, without
    * mipmapping, multisampling, block-compression, or arrays.
    */
   AIL_TILING_LINEAR,

   /**
    * GPU-tiled. Always allowed.
    */
   AIL_TILING_GPU,
};

/*
 * Represents the dimensions of a single tile. Used to describe tiled layouts.
 * Width and height are in units of elements, not pixels, to model compressed
 * textures corrects.
 *
 * Invariant: width_el and height_el are powers of two.
 */
struct ail_tile {
   unsigned width_el, height_el;
};

/*
 * An AGX image layout.
 */
struct ail_layout {
   /** Width, height, and depth in pixels at level 0 */
   uint32_t width_px, height_px, depth_px;

   /** Number of samples per pixel. 1 if multisampling is disabled. */
   uint8_t sample_count_sa;

   /** Number of miplevels. 1 if no mipmapping is used. */
   uint8_t levels;

   /** Should this image be mipmapped along the Z-axis in addition to the X- and
    * Y-axes? This should be set for API-level 3D images, but not 2D arrays or
    * cubes.
    */
   bool mipmapped_z;

   /** Tiling mode used */
   enum ail_tiling tiling;

   /** Whether compression is used. Requires a non-linear layout. */
   bool compressed;

   /** Texture format */
   enum pipe_format format;

   /**
    * If tiling is LINEAR, the number of bytes between adjacent rows of
    * elements. Otherwise, this field is zero.
    */
   uint32_t linear_stride_B;

   /**
    * Stride between layers of an array texture, including a cube map. Layer i
    * begins at offset (i * layer_stride_B) from the beginning of the texture.
    *
    * If depth_px = 1, the value of this field is UNDEFINED.
    */
   uint64_t layer_stride_B;

   /**
    * Whether the layer stride is aligned to the page size or not. The hardware
    * needs this flag to compute the implicit layer stride.
    */
   bool page_aligned_layers;

   /**
    * Offsets of mip levels within a layer.
    */
   uint64_t level_offsets_B[AIL_MAX_MIP_LEVELS];

   /**
    * For the compressed buffer, offsets of mip levels within a layer.
    */
   uint64_t level_offsets_compressed_B[AIL_MAX_MIP_LEVELS];

   /**
    * If tiling is nonlinear, the tile size used for each mip level within a
    * layer. Calculating tile sizes is the sole responsibility of
    * ail_initialized_twiddled.
    */
   struct ail_tile tilesize_el[AIL_MAX_MIP_LEVELS];

   /**
    * If tiling is nonlinear, the stride in elements used for each mip level
    * within a layer. Calculating level strides is the sole responsibility of
    * ail_initialized_twiddled. This is necessary because compressed pixel
    * formats may add extra stride padding.
    */
   uint32_t stride_el[AIL_MAX_MIP_LEVELS];

   /**
    * If the image is bound sparsely, the first LOD/level of the miptail.
    * This corresponds to Vulkan imageMipTailFirstLod.
    */
   uint32_t mip_tail_first_lod;

   /**
    * If the image is bound sparsely, the layer stride of the miptail.
    * Conceptually, this corresponds to Vulkan imageMipTailStride. However, it
    * is not actually used for imageMipTailStride due to complications with
    * standard sparse block sizes.
    */
   uint32_t mip_tail_stride;

   /* Offset of the start of the compression metadata buffer */
   uint32_t metadata_offset_B;

   /* Stride between subsequent layers in the compression metadata buffer */
   uint64_t compression_layer_stride_B;

   /* Size of entire texture */
   uint64_t size_B;

   /* Size of the sparse page table required to address this image. This is
    * calculated for all images (whether sparse is used or not), since the
    * hardware has few requirements imposed on sparse.
    *
    * This size does not contribute to size_B. In a sparse image, size_B is the
    * logical size of the image (if it were completely resident), whereas
    * sparse_table_size_B is the physical size of the page table required to
    * address that logical image.
    */
   uint64_t sparse_table_size_B;

   /* Number of sparse folios required to cover a single layer of an image. This
    * is proportional to sparse_table_size_B but split out for convenience.
    */
   uint32_t sparse_folios_per_layer;

   /* Must the layout support writeable images? If false, the layout MUST NOT be
    * used as a writeable image (either PBE or image atomics).
    */
   bool writeable_image;

   /* Must the layout support rendering? If false, the layout MUST NOT be used
    * for rendering, either PBE or ZLS.
    */
   bool renderable;
};

static inline uint32_t
ail_get_linear_stride_B(const struct ail_layout *layout, ASSERTED uint8_t level)
{
   assert(layout->tiling == AIL_TILING_LINEAR && "Invalid usage");
   assert(level == 0 && "Strided linear mipmapped textures are unsupported");

   return layout->linear_stride_B;
}

/*
 * For WSI purposes, we need to associate a stride with all layouts. In the
 * hardware, only strided linear images have an associated stride, there is no
 * natural stride associated with nonlinear images. However, various clients
 * assert that the stride is valid for the image if it were linear (even if it
 * is in fact not linear). In those cases, by convention we use the minimum
 * valid such stride.
 */
static inline uint32_t
ail_get_wsi_stride_B(const struct ail_layout *layout, unsigned level)
{
   assert(level == 0 && "Mipmaps cannot be shared as WSI");

   if (layout->tiling == AIL_TILING_LINEAR)
      return ail_get_linear_stride_B(layout, level);
   else
      return util_format_get_stride(layout->format, layout->width_px);
}

static inline uint32_t
ail_get_layer_offset_B(const struct ail_layout *layout, unsigned z_px)
{
   return z_px * layout->layer_stride_B;
}

static inline uint32_t
ail_get_level_offset_B(const struct ail_layout *layout, unsigned level)
{
   return layout->level_offsets_B[level];
}

static inline uint32_t
ail_get_layer_level_B(const struct ail_layout *layout, unsigned z_px,
                      unsigned level)
{
   return ail_get_layer_offset_B(layout, z_px) +
          ail_get_level_offset_B(layout, level);
}

static inline uint32_t
ail_get_level_size_B(const struct ail_layout *layout, unsigned level)
{
   if (layout->tiling == AIL_TILING_LINEAR) {
      assert(level == 0);
      return layout->layer_stride_B;
   } else {
      assert(level + 1 < ARRAY_SIZE(layout->level_offsets_B));
      return layout->level_offsets_B[level + 1] -
             layout->level_offsets_B[level];
   }
}

static inline uint32_t
ail_get_linear_pixel_B(const struct ail_layout *layout, ASSERTED unsigned level,
                       uint32_t x_px, uint32_t y_px, uint32_t z_px)
{
   assert(level == 0 && "Strided linear mipmapped textures are unsupported");
   assert(util_format_get_blockwidth(layout->format) == 1 &&
          "Strided linear block formats unsupported");
   assert(util_format_get_blockheight(layout->format) == 1 &&
          "Strided linear block formats unsupported");
   assert(layout->sample_count_sa == 1 &&
          "Strided linear multisampling unsupported");

   return ail_get_layer_offset_B(layout, z_px) +
          (y_px * ail_get_linear_stride_B(layout, level)) +
          (x_px * util_format_get_blocksize(layout->format));
}

static inline uint32_t
ail_space_bits(unsigned x)
{
   assert(x < 128 && "offset must be inside the tile");

   return ((x & 1) << 0) | ((x & 2) << 1) | ((x & 4) << 2) | ((x & 8) << 3) |
          ((x & 16) << 4) | ((x & 32) << 5) | ((x & 64) << 6);
}

#define MOD_POT(x, y) (x) & ((y) - 1)

static inline uint32_t
ail_get_blocksize_B(const struct ail_layout *layout)
{
   return util_format_get_blocksize(layout->format);
}

static inline uint32_t
ail_get_twiddled_block_B(const struct ail_layout *layout, unsigned level,
                         uint32_t x_px, uint32_t y_px, uint32_t z_px)
{
   assert(layout->tiling == AIL_TILING_GPU);
   assert(level < layout->levels);

   unsigned x_el = util_format_get_nblocksx(layout->format, x_px);
   unsigned y_el = util_format_get_nblocksy(layout->format, y_px);

   struct ail_tile tile_size = layout->tilesize_el[level];
   assert((x_el % tile_size.width_el) == 0 && "must be aligned");
   assert((y_el % tile_size.height_el) == 0 && "must be aligned");

   unsigned stride_el = layout->stride_el[level];
   unsigned tile_area_el = tile_size.width_el * tile_size.height_el;
   unsigned tiles_per_row = DIV_ROUND_UP(stride_el, tile_size.width_el);

   unsigned y_rowtile = y_el / tile_size.height_el;
   unsigned y_tile = y_rowtile * tiles_per_row;

   unsigned tile_idx = y_tile + (x_el / tile_size.width_el);
   unsigned tile_offset_el = tile_idx * tile_area_el;
   unsigned tile_offset_B = tile_offset_el * ail_get_blocksize_B(layout);

   return ail_get_layer_level_B(layout, z_px, level) + tile_offset_B;
}

static inline unsigned
ail_effective_width_sa(unsigned width_px, unsigned sample_count_sa)
{
   return width_px * (sample_count_sa == 4 ? 2 : 1);
}

static inline unsigned
ail_effective_height_sa(unsigned height_px, unsigned sample_count_sa)
{
   return height_px * (sample_count_sa >= 2 ? 2 : 1);
}

static inline unsigned
ail_metadata_width_tl(struct ail_layout *layout, unsigned level)
{
   unsigned px = u_minify(layout->width_px, level);
   uint32_t sa = ail_effective_width_sa(px, layout->sample_count_sa);
   return DIV_ROUND_UP(sa, 16);
}

static inline unsigned
ail_metadata_height_tl(struct ail_layout *layout, unsigned level)
{
   unsigned px = u_minify(layout->height_px, level);
   uint32_t sa = ail_effective_height_sa(px, layout->sample_count_sa);
   return DIV_ROUND_UP(sa, 16);
}

/*
 * Even when the base mip level is compressed, high levels of the miptree
 * (smaller than 16 pixels on either axis) are not compressed as it would be
 * pointless. This queries this case.
 */
static inline bool
ail_is_level_compressed(const struct ail_layout *layout, unsigned level)
{
   unsigned width_sa = ALIGN(
      ail_effective_width_sa(layout->width_px, layout->sample_count_sa), 16);

   unsigned height_sa = ALIGN(
      ail_effective_height_sa(layout->height_px, layout->sample_count_sa), 16);

   return layout->compressed &&
          u_minify(MAX2(width_sa, height_sa), level) >= 16;
}

static inline bool
ail_is_level_twiddled_uncompressed(const struct ail_layout *layout,
                                   unsigned level)
{
   if (layout->compressed) {
      return !ail_is_level_compressed(layout, level);
   } else {
      return layout->tiling != AIL_TILING_LINEAR;
   }
}

struct ail_tile ail_get_max_tile_size(unsigned blocksize_B);

void ail_make_miptree(struct ail_layout *layout);

void ail_detile(void *_tiled, void *_linear,
                const struct ail_layout *tiled_layout, unsigned level,
                unsigned linear_pitch_B, unsigned sx_px, unsigned sy_px,
                unsigned width_px, unsigned height_px);

void ail_tile(void *_tiled, void *_linear,
              const struct ail_layout *tiled_layout, unsigned level,
              unsigned linear_pitch_B, unsigned sx_px, unsigned sy_px,
              unsigned width_px, unsigned height_px);

/* Define aliases for the subset formats that are accessible in the ISA. These
 * subsets disregard component mapping and number of components. This
 * constitutes ABI with the compiler.
 */
enum ail_isa_format {
   AIL_ISA_FORMAT_I8 = PIPE_FORMAT_R8_UINT,
   AIL_ISA_FORMAT_I16 = PIPE_FORMAT_R16_UINT,
   AIL_ISA_FORMAT_I32 = PIPE_FORMAT_R32_UINT,
   AIL_ISA_FORMAT_F16 = PIPE_FORMAT_R16_FLOAT,
   AIL_ISA_FORMAT_U8NORM = PIPE_FORMAT_R8_UNORM,
   AIL_ISA_FORMAT_S8NORM = PIPE_FORMAT_R8_SNORM,
   AIL_ISA_FORMAT_U16NORM = PIPE_FORMAT_R16_UNORM,
   AIL_ISA_FORMAT_S16NORM = PIPE_FORMAT_R16_SNORM,
   AIL_ISA_FORMAT_RGB10A2 = PIPE_FORMAT_R10G10B10A2_UNORM,
   AIL_ISA_FORMAT_SRGBA8 = PIPE_FORMAT_R8G8B8A8_SRGB,
   AIL_ISA_FORMAT_RG11B10F = PIPE_FORMAT_R11G11B10_FLOAT,
   AIL_ISA_FORMAT_RGB9E5 = PIPE_FORMAT_R9G9B9E5_FLOAT
};

/*
 * The architecture load/store instructions support masking, but packed formats
 * are not compatible with masking. Check if a format is packed.
 */
static inline bool
ail_isa_format_supports_mask(enum ail_isa_format format)
{
   switch (format) {
   case AIL_ISA_FORMAT_RGB10A2:
   case AIL_ISA_FORMAT_RG11B10F:
   case AIL_ISA_FORMAT_RGB9E5:
      return false;
   default:
      return true;
   }
}

struct ail_pixel_format_entry {
   uint8_t channels;
   uint8_t type;
   bool texturable : 1;
   enum pipe_format renderable;
};

extern const struct ail_pixel_format_entry ail_pixel_format[PIPE_FORMAT_COUNT];

static inline bool
ail_is_valid_pixel_format(enum pipe_format format)
{
   return ail_pixel_format[format].texturable;
}

/* Query whether an image with the specified layout is compressible */
static inline bool
ail_can_compress(enum pipe_format format, unsigned w_px, unsigned h_px,
                 unsigned sample_count_sa)
{
   assert(sample_count_sa == 1 || sample_count_sa == 2 || sample_count_sa == 4);

   /* We compress via the PBE, so we can only compress PBE-writeable formats. */
   if (ail_pixel_format[format].renderable == PIPE_FORMAT_NONE &&
       !util_format_is_depth_or_stencil(format))
      return false;

   /* Lossy-compressed texture formats cannot be compressed */
   assert(!util_format_is_compressed(format) &&
          "block-compressed formats are not renderable");

   /* Small textures cannot be compressed */
   return ail_effective_width_sa(w_px, sample_count_sa) >= 16 &&
          ail_effective_height_sa(h_px, sample_count_sa) >= 16;
}

/* AGX compression mode for a solid colour for the subtile */
#define AIL_COMP_SOLID 0x3

/* AGX compression mode for an uncompessed subtile. Frustratingly, this seems to
 * depend on the format. It is possible that modes are actual 8-bit structures
 * with multiple fields rather than plain enumerations.
 */
#define AIL_COMP_UNCOMPRESSED_1    0x1f
#define AIL_COMP_UNCOMPRESSED_2    0x3f
#define AIL_COMP_UNCOMPRESSED_4    0x7f
#define AIL_COMP_UNCOMPRESSED_8_16 0xff

static inline uint8_t
ail_subtile_uncompressed_mode(enum pipe_format format)
{
   /* clang-format off */
   switch (util_format_get_blocksize(format)) {
   case  1: return AIL_COMP_UNCOMPRESSED_1;
   case  2: return AIL_COMP_UNCOMPRESSED_2;
   case  4: return AIL_COMP_UNCOMPRESSED_4;
   case  8:
   case 16: return AIL_COMP_UNCOMPRESSED_8_16;
   default: unreachable("invalid block size");
   }
   /* clang-format on */
}

/*
 * Compression modes are 8-bit per 8x4 subtile, but grouped into 64-bit for all
 * modes in a 16x16 tile. This helper replicates a subtile mode to a tile mode
 * using a SWAR idiom.
 */
static inline uint64_t
ail_tile_mode_replicated(uint8_t subtile_mode)
{
   return (uint64_t)subtile_mode * 0x0101010101010101ULL;
}

/*
 * Composed convenience function.
 */
static inline uint64_t
ail_tile_mode_uncompressed(enum pipe_format format)
{
   return ail_tile_mode_replicated(ail_subtile_uncompressed_mode(format));
}

/*
 * For compression, compatible formats must have the same number/size/order of
 * channels, but may differ in data type. For example, R32_SINT is compatible
 * with Z32_FLOAT, but not with R16G16_SINT. This is the relation given by the
 * "channels" part of the decomposed format.
 *
 * This has not been exhaustively tested and might be missing some corner cases
 * around XR formats, but is well-motivated and seems to work.
 */
static inline bool
ail_formats_compatible(enum pipe_format a, enum pipe_format b)
{
   return ail_pixel_format[a].channels == ail_pixel_format[b].channels;
}

static inline bool
ail_is_view_compatible(struct ail_layout *layout, enum pipe_format view)
{
   return !layout->compressed || ail_formats_compatible(layout->format, view);
}

/* Fake values, pending UAPI upstreaming */
#ifndef DRM_FORMAT_MOD_APPLE_GPU_TILED
#define DRM_FORMAT_MOD_APPLE_GPU_TILED (2)
#endif
#ifndef DRM_FORMAT_MOD_APPLE_GPU_TILED_COMPRESSED
#define DRM_FORMAT_MOD_APPLE_GPU_TILED_COMPRESSED (3)
#endif

/*
 * We generally use ail enums instead of DRM format modifiers. These helpers
 * bridges the gap.
 */
static inline enum ail_tiling
ail_drm_modifier_to_tiling(uint64_t modifier)
{
   switch (modifier) {
   case DRM_FORMAT_MOD_LINEAR:
      return AIL_TILING_LINEAR;
   case DRM_FORMAT_MOD_APPLE_GPU_TILED:
   case DRM_FORMAT_MOD_APPLE_GPU_TILED_COMPRESSED:
      return AIL_TILING_GPU;
   default:
      unreachable("Unsupported modifier");
   }
}

static inline bool
ail_is_drm_modifier_compressed(uint64_t modifier)
{
   switch (modifier) {
   case DRM_FORMAT_MOD_LINEAR:
   case DRM_FORMAT_MOD_APPLE_GPU_TILED:
      return false;
   case DRM_FORMAT_MOD_APPLE_GPU_TILED_COMPRESSED:
      return true;
   default:
      unreachable("Unsupported modifier");
   }
}

/*
 * Constants for sparse page tables. See docs/drivers/asahi.rst.
 */
#define AIL_SPARSE_ELSIZE_B        4
#define AIL_PAGES_PER_FOLIO        256
#define AIL_IMAGE_SIZE_PER_FOLIO_B (AIL_PAGESIZE * AIL_PAGES_PER_FOLIO)
#define AIL_FOLIO_SIZE_EL          (AIL_PAGES_PER_FOLIO * 2)
#define AIL_FOLIO_SIZE_B           (AIL_FOLIO_SIZE_EL * AIL_SPARSE_ELSIZE_B)

static inline unsigned
ail_bytes_to_pages(unsigned x_B)
{
   assert((x_B % AIL_PAGESIZE) == 0);
   return x_B / AIL_PAGESIZE;
}

/*
 * Map a logical page of the image (i.e. page = logical address / page_size) to
 * a word-index into the sparse table.
 */
static inline unsigned
ail_page_to_sparse_index_el(const struct ail_layout *layout, unsigned z,
                            unsigned page)
{
   unsigned z_base = z * layout->sparse_folios_per_layer;
   unsigned folio = page / AIL_PAGES_PER_FOLIO;
   unsigned index_in_folio = page % AIL_PAGES_PER_FOLIO;
   unsigned idx = ((z_base + folio) * AIL_FOLIO_SIZE_EL) + index_in_folio;

   assert((idx * AIL_SPARSE_ELSIZE_B) < layout->sparse_table_size_B);
   return idx;
}

#ifdef __cplusplus
} /* extern C */
#endif
