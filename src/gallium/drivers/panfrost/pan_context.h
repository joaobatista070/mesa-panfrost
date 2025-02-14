/*
 * © Copyright 2018 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __BUILDER_H__
#define __BUILDER_H__

#define BIT64
#define MFBD

#define _LARGEFILE64_SOURCE 1
#define CACHE_LINE_SIZE 1024 /* TODO */
#include <sys/mman.h>
#include <assert.h>
#include "pan_resource.h"

#include "pipe/p_compiler.h"
#include "pipe/p_config.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_format.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_blitter.h"

/* Forward declare to avoid extra header dep */
struct prim_convert_context;

/* TODO: Handle on newer hardware */
#ifdef MFBD
#define PANFROST_DEFAULT_FBD (MALI_MFBD)
#define PANFROST_FRAMEBUFFER struct bifrost_framebuffer
#else
#define PANFROST_DEFAULT_FBD (MALI_SFBD)
#define PANFROST_FRAMEBUFFER struct mali_single_framebuffer
#endif

#define MAX_DRAW_CALLS 4096
#define MAX_VARYINGS   4096

//#define PAN_DIRTY_CLEAR	     (1 << 0)
#define PAN_DIRTY_RASTERIZER (1 << 2)
#define PAN_DIRTY_FS	     (1 << 3)
#define PAN_DIRTY_FRAG_CORE  (PAN_DIRTY_FS) /* Dirty writes are tied */
#define PAN_DIRTY_VS	     (1 << 4)
#define PAN_DIRTY_VERTEX     (1 << 5)
#define PAN_DIRTY_VERT_BUF   (1 << 6)
//#define PAN_DIRTY_VIEWPORT   (1 << 7)
#define PAN_DIRTY_SAMPLERS   (1 << 8)
#define PAN_DIRTY_TEXTURES   (1 << 9)

struct panfrost_constant_buffer {
        bool dirty;
        size_t size;
        void *buffer;
};

struct panfrost_query {
        /* Passthrough from Gallium */
        unsigned type;
        unsigned index;

        /* Memory for the GPU to writeback the value of the query */
        struct panfrost_transfer transfer;
};

#define PANFROST_MAX_TRANSIENT_ENTRIES 64

struct panfrost_transient_pool {
        /* Memory blocks in the pool */
        struct panfrost_memory_entry *entries[PANFROST_MAX_TRANSIENT_ENTRIES];

        /* Number of entries we own */
        unsigned entry_count;

        /* Current entry that we are writing to, zero-indexed, strictly less than entry_count */
        unsigned entry_index;

        /* Number of bytes into the current entry we are */
        off_t entry_offset;

        /* Entry size (all entries must be homogenous) */
        size_t entry_size;
};

struct panfrost_context {
        /* Gallium context */
        struct pipe_context base;

        struct pipe_framebuffer_state pipe_framebuffer;

        /* The number of concurrent FBOs allowed depends on the number of pools
         * used; pools are ringed for parallelism opportunities */

        struct panfrost_transient_pool transient_pools[2];
        int cmdstream_i;

        struct panfrost_memory cmdstream_persistent;
        struct panfrost_memory shaders;
        struct panfrost_memory scratchpad;
        struct panfrost_memory tiler_heap;
        struct panfrost_memory varying_mem;
        struct panfrost_memory misc_0;
        struct panfrost_memory misc_1;
        struct panfrost_memory depth_stencil_buffer;

        struct {
                unsigned buffers;
                const union pipe_color_union *color;
                double depth;
                unsigned stencil;
        } last_clear;

        struct panfrost_query *occlusion_query;

        /* Each render job has multiple framebuffer descriptors associated with
         * it, used for various purposes with more or less the same format. The
         * most obvious is the fragment framebuffer descriptor, which carries
         * e.g. clearing information */

#ifdef SFBD
        struct mali_single_framebuffer fragment_fbd;
#else
        struct bifrost_framebuffer fragment_fbd;

        struct bifrost_fb_extra fragment_extra;

        struct bifrost_render_target fragment_rts[4];
#endif

        /* Each draw has corresponding vertex and tiler payloads */
        struct midgard_payload_vertex_tiler payload_vertex;
        struct midgard_payload_vertex_tiler payload_tiler;

        /* The fragment shader binary itself is pointed here (for the tripipe) but
         * also everything else in the shader core, including blending, the
         * stencil/depth tests, etc. Refer to the presentations. */

        struct mali_shader_meta fragment_shader_core;

        /* A frame is composed of a starting set value job, a number of vertex
         * and tiler jobs, linked to the fragment job at the end. See the
         * presentations for more information how this works */

        unsigned draw_count;

        mali_ptr set_value_job;
        mali_ptr vertex_jobs[MAX_DRAW_CALLS];
        mali_ptr tiler_jobs[MAX_DRAW_CALLS];

        struct mali_job_descriptor_header *u_set_value_job;
        struct mali_job_descriptor_header *u_vertex_jobs[MAX_DRAW_CALLS];
        struct mali_job_descriptor_header *u_tiler_jobs[MAX_DRAW_CALLS];

        unsigned vertex_job_count;
        unsigned tiler_job_count;

        /* Per-draw Dirty flags are setup like any other driver */
        int dirty;

        /* Per frame dirty flag - whether there was a clear. If not, we need to do a partial update, maybe */
        bool frame_cleared;

        unsigned vertex_count;

        union mali_attr attributes[PIPE_MAX_ATTRIBS];

        unsigned varying_height;

        struct mali_viewport *viewport;
        PANFROST_FRAMEBUFFER vt_framebuffer;

        /* TODO: Multiple uniform buffers (index =/= 0), finer updates? */

        struct panfrost_constant_buffer constant_buffer[PIPE_SHADER_TYPES];

        /* CSOs */
        struct panfrost_rasterizer *rasterizer;

        struct panfrost_shader_variants *vs;
        struct panfrost_shader_variants *fs;

        struct panfrost_vertex_state *vertex;

        struct pipe_vertex_buffer *vertex_buffers;
        unsigned vertex_buffer_count;

        struct panfrost_sampler_state *samplers[PIPE_SHADER_TYPES][PIPE_MAX_SAMPLERS];
        unsigned sampler_count[PIPE_SHADER_TYPES];

        struct panfrost_sampler_view *sampler_views[PIPE_SHADER_TYPES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
        unsigned sampler_view_count[PIPE_SHADER_TYPES];

        struct primconvert_context *primconvert;
        struct blitter_context *blitter;

        struct panfrost_blend_state *blend;

        struct pipe_viewport_state pipe_viewport;
        struct pipe_scissor_state scissor;
        struct pipe_blend_color blend_color;
        struct pipe_depth_stencil_alpha_state *depth_stencil;
        struct pipe_stencil_ref stencil_ref;

        /* Memory management is based on subdividing slabs with AMD's allocator */
        struct pb_slabs slabs;
};

/* Corresponds to the CSO */

struct panfrost_rasterizer {
        struct pipe_rasterizer_state base;

        /* Bitmask of front face, etc */
        unsigned tiler_gl_enables;
};

struct panfrost_blend_state {
        struct pipe_blend_state base;

        /* Whether a blend shader is in use */
        bool has_blend_shader;

        /* Compiled fixed function command */
        struct mali_blend_equation equation;

        /* Compiled blend shader */
        mali_ptr blend_shader;
        int blend_work_count;
};

/* Internal varyings descriptor */
struct panfrost_varyings {
        /* Varyings information: stride of each chunk of memory used for
         * varyings (similar structure with attributes). Count is just the
         * number of vec4's. Buffer count is the number of varying chunks (<=
         * count). Height is used to calculate gl_Position's position ("it's
         * not a pun, Alyssa!"). Vertex-only varyings == descriptor for
         * gl_Position and something else apparently occupying the same space.
         * Varyings == main varyings descriptors following typical mali_attr
         * conventions. */

        unsigned varyings_stride[MAX_VARYINGS];
        unsigned varying_count;
        unsigned varying_buffer_count;

        /* Map of the actual varyings buffer */
        uint8_t *varyings_buffer_cpu;
        mali_ptr varyings_descriptor;
        mali_ptr varyings_descriptor_fragment;
};

/* Variants bundle together to form the backing CSO, bundling multiple
 * shaders with varying emulated features baked in (alpha test
 * parameters, etc) */
#define MAX_SHADER_VARIANTS 8

/* A shader state corresponds to the actual, current variant of the shader */
struct panfrost_shader_state {
        struct pipe_shader_state *base;

        /* Compiled, mapped descriptor, ready for the hardware */
        bool compiled;
        struct mali_shader_meta *tripipe;
        mali_ptr tripipe_gpu;

        /* Non-descript information */
        int uniform_count;
        bool can_discard;

        /* Valid for vertex shaders only due to when this is calculated */
        struct panfrost_varyings varyings;

        /* Information on this particular shader variant */
        struct pipe_alpha_state alpha_state;
};

/* A collection of varyings (the CSO) */
struct panfrost_shader_variants {
        struct pipe_shader_state base;

        struct panfrost_shader_state variants[MAX_SHADER_VARIANTS];
        unsigned variant_count;

        /* The current active variant */
        unsigned active_variant;
};

struct panfrost_vertex_state {
        unsigned num_elements;

        struct pipe_vertex_element pipe[PIPE_MAX_ATTRIBS];
        int nr_components[PIPE_MAX_ATTRIBS];

        /* The actual attribute meta, prebaked and GPU mapped. TODO: Free memory */
        struct mali_attr_meta *hw;
        mali_ptr descriptor_ptr;
};

struct panfrost_sampler_state {
        struct pipe_sampler_state base;
        struct mali_sampler_descriptor hw;
};

/* Misnomer: Sampler view corresponds to textures, not samplers */

struct panfrost_sampler_view {
        struct pipe_sampler_view base;
        struct mali_texture_descriptor hw;
};

static inline struct panfrost_context *
pan_context(struct pipe_context *pcontext)
{
        return (struct panfrost_context *) pcontext;
}

static inline struct panfrost_screen *
pan_screen(struct pipe_screen *p)
{
   return (struct panfrost_screen *)p;
}

struct pipe_context *
panfrost_create_context(struct pipe_screen *screen, void *priv, unsigned flags);

void
panfrost_emit_for_draw(struct panfrost_context *ctx, bool with_vertex_data);

struct panfrost_transfer
panfrost_vertex_tiler_job(struct panfrost_context *ctx, bool is_tiler, bool is_elided_tiler);

unsigned
panfrost_get_default_swizzle(unsigned components);

void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags);

void
panfrost_shader_compile(struct panfrost_context *ctx, struct mali_shader_meta *meta, const char *src, int type, struct panfrost_shader_state *state);

#endif
