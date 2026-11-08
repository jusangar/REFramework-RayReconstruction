// The guide pass and its debug view, and the kernels of the measurement interface. dinput8.dll embeds this
// file and Guides.hlsli, and compiles each kernel at run time with D3DCompile, cs_5_0. The per-pixel math
// is in Guides.hlsli, which the tests of the RR core run as C++; here there are only the loads and the
// stores.

#include "Guides.hlsli"

// --- The game's AmbientBRDF LUT ---

// Root constants.
cbuffer LutConstants : register(b2) {
    uint2 lut_size; // the profile's
};

// What the PT samples as AmbientBRDF, through its own descriptor on the game's heap. The PT declares it as
// a Texture2D, so it is read as one.
Texture2D<float4> game_ambient_brdf : register(t6);

// RR's copy, through a root UAV: the texels row by row, then the size of the texture found.
RWStructuredBuffer<float2> ambient_brdf_out : register(u6);

// A texture of another size is not the LUT: the copy keeps only its size, and the pass falls back on
// the GGX fit.
[numthreads(8, 8, 1)]
void copy_ambient_brdf(uint3 id : SV_DispatchThreadID) {
    uint width;
    uint height;
    game_ambient_brdf.GetDimensions(width, height);

    if (id.x < lut_size.x && id.y < lut_size.y) {
        float2 texel = float2(0.0f, 0.0f);

        if (width == lut_size.x && height == lut_size.y) {
            texel = game_ambient_brdf.Load(int3(id.xy, 0)).xy;
        }

        ambient_brdf_out[id.y * lut_size.x + id.x] = texel;
    }

    if (id.x == 0 && id.y == 0) {
        ambient_brdf_out[lut_size.x * lut_size.y] = float2((float)width, (float)height);
    }
}

// --- The guide pass ---

// Root constants. The matrices are copied as they are in memory, so the default column_major packing
// gives HLSL the same matrices (Guides.hlsli).
cbuffer GuideConstants : register(b0) {
    float4x4 guide_inv_view_proj;   // Scene's, from the camera update of the decision
    float2 guide_render_size;       // T's
    float2 guide_jitter;            // j, in render pixels
    float4x4 guide_old_view_proj;   // Scene's old_view_projection, from the same update
    uint guide_motion;              // MOTION_* of Guides.hlsli
    uint2 guide_lut_size;           // the profile's
    uint guide_lut;                 // 1 if copy_ambient_brdf ran before the pass
};

// The game's resources, read in their state at the TAA draw, without barriers.
Texture2D<float4> albedo_in : register(t0);   // RT1 of the G-buffer pass, through an _SRGB view
Texture2D<float4> normals_in : register(t1);  // RT2
Texture2D<float4> velocity_in : register(t2); // RT3; a null descriptor in a frame without velocity
Texture2D<float> depth_in : register(t3);     // plane 0 of the depth

// copy_ambient_brdf's buffer. Read only with guide_lut: otherwise it is not in a state to be read.
StructuredBuffer<float2> ambient_brdf_in : register(t4);

// RR's own textures: what NGX takes.
RWTexture2D<float4> normal_roughness_out : register(u0);
RWTexture2D<float4> diffuse_out : register(u1);
RWTexture2D<float4> specular_out : register(u2);
RWTexture2D<float> depth_out : register(u3);
RWTexture2D<float2> motion_vectors_out : register(u4);

// As SampleLevel with a linear clamp sampler would sample the LUT.
float2 sample_ambient_brdf(float2 uv) {
    BilinearFootprint f = bilinear_footprint(uv, float2(guide_lut_size));
    uint near_row = (uint)f.near_texel.y * guide_lut_size.x;
    uint far_row = (uint)f.far_texel.y * guide_lut_size.x;
    uint near_column = (uint)f.near_texel.x;
    uint far_column = (uint)f.far_texel.x;

    return bilinear(ambient_brdf_in[near_row + near_column], ambient_brdf_in[near_row + far_column],
        ambient_brdf_in[far_row + near_column], ambient_brdf_in[far_row + far_column], f.weights);
}

[numthreads(8, 8, 1)]
void guide_pass(uint3 id : SV_DispatchThreadID) {
    if (id.x < (uint)guide_render_size.x && id.y < (uint)guide_render_size.y) {
        int3 p = int3(id.xy, 0);
        Surface s = surface_at(float2(id.x + 0.5f, id.y + 0.5f), normals_in.Load(p), albedo_in.Load(p), depth_in.Load(p), guide_render_size,
            guide_inv_view_proj);

        // The copy ends with the size of the texture at the PT's register.
        bool has_lut = false;
        float2 lut = float2(0.0f, 0.0f);

        if (guide_lut != 0) {
            float2 found = ambient_brdf_in[guide_lut_size.x * guide_lut_size.y];
            has_lut = found.x == (float)guide_lut_size.x && found.y == (float)guide_lut_size.y;
        }

        if (has_lut) {
            lut = sample_ambient_brdf(ambient_brdf_uv(s.n_dot_v, s.material.roughness));
        }

        int motion = (int)guide_motion;
        float2 velocity = float2(0.0f, 0.0f);

        if (motion != MOTION_REPROJECT_ALL) {
            velocity = velocity_in.Load(p).xy;
        }

        GuidePixel g = guide_pixel(s, has_lut, lut, velocity, guide_render_size, guide_jitter, guide_old_view_proj, motion);

        normal_roughness_out[id.xy] = g.guides.normal_roughness;
        diffuse_out[id.xy] = float4(g.guides.diffuse, 1.0f);
        specular_out[id.xy] = float4(g.guides.specular, 1.0f);
        depth_out[id.xy] = g.depth;
        motion_vectors_out[id.xy] = g.motion_vector;
    }
}

// --- The debug view ---

cbuffer DebugConstants : register(b1) {
    uint debug_guide; // DEBUG_VIEW_* of Guides.hlsli
    float debug_scale;
    uint2 debug_size; // O's, which is T's
};

Texture2D<float4> debug_source : register(t5);   // the guide's texture
RWTexture2D<float4> debug_target : register(u5); // O

[numthreads(8, 8, 1)]
void debug_view(uint3 id : SV_DispatchThreadID) {
    if (id.x < debug_size.x && id.y < debug_size.y) {
        debug_target[id.xy] = float4(debug_view_color((int)debug_guide, debug_source.Load(int3(id.xy, 0)), debug_scale), 1.0f);
    }
}

// --- The accumulated reference ---

cbuffer ReferenceConstants : register(b3) {
    uint2 reference_size;          // T's, which is O's
    uint reference_frames_before;  // the frames the mean holds: 0 starts it with this one
};

Texture2D<float4> reference_t : register(t7);           // T, in its state at the TAA draw
RWTexture2D<float4> reference_o : register(u7);          // O
RWStructuredBuffer<float4> reference_mean : register(u8); // root UAV: accumulate_mean's, row by row at T's width

// This frame's T joins the mean of T, which goes into O in place of the TAA, the way RR's output does.
[numthreads(8, 8, 1)]
void accumulate_reference(uint3 id : SV_DispatchThreadID) {
    if (id.x < reference_size.x && id.y < reference_size.y) {
        uint i = id.y * reference_size.x + id.x;
        float4 t = reference_t.Load(int3(id.xy, 0));
        float4 held = accumulate_mean(reference_mean[i], t.xyz, (float)reference_frames_before);

        reference_mean[i] = held;
        reference_o[id.xy] = float4(held.xyz, 1.0f);
    }
}

// --- The count of non-finite values in O ---

cbuffer CountConstants : register(b4) {
    uint2 count_size;    // O's
    uint count_forced;   // if not 0, the bits of the value pixel (0, 0) takes for the count: the debug test
};

Texture2D<float4> count_source : register(t8);        // O, after RR's evaluation
RWStructuredBuffer<uint> nonfinite_totals : register(u9); // root UAV: the NaN pixels, then the infinite ones, never cleared

groupshared uint group_nan;
groupshared uint group_inf;

// Each group counts its pixels in shared memory, and only a group that found some adds them to the
// totals: in normal play, no atomic reaches memory.
[numthreads(8, 8, 1)]
void count_nonfinite(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) {
        group_nan = 0;
        group_inf = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    if (id.x < count_size.x && id.y < count_size.y) {
        float3 c = count_source.Load(int3(id.xy, 0)).xyz;

        if (count_forced != 0 && id.x == 0 && id.y == 0) {
            c.x = asfloat(count_forced);
        }

        int kind = nonfinite_kind(c);

        if (kind == NONFINITE_NAN) {
            InterlockedAdd(group_nan, 1u);
        } else if (kind == NONFINITE_INF) {
            InterlockedAdd(group_inf, 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (index == 0) {
        if (group_nan > 0) {
            InterlockedAdd(nonfinite_totals[0], group_nan);
        }

        if (group_inf > 0) {
            InterlockedAdd(nonfinite_totals[1], group_inf);
        }
    }
}
