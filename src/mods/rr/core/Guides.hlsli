#ifndef RR_GUIDES_HLSLI
#define RR_GUIDES_HLSLI

// The per-pixel math of the guide pass, and of the kernels of the measurement interface. The compute
// shader embeds this file in its HLSL source, and the tests of the RR core compile it as C++ through
// Guides.hpp, so that they test the code that runs on the GPU.
//
// It keeps to what FXC (cs_5_0) and C++ share, and includes nothing:
//
// - float2, float3, float4 and float4x4, with full constructors and no swizzles: only .x .y .z .w;
// - scalar ternaries, static const scalars, and inline functions without out parameters or early
//   returns;
// - uint, and the intrinsics HlslCompat.hpp gives C++.
//
// Conventions:
//
// - pixels from the top left corner, Y down; NDC with Y up; depth reversed, 0 at the far plane;
// - matrices as REFramework reads the game's (Matrix4x4f, glm): clip = M * position. Copied as they
//   are in memory into a cbuffer with the default column_major packing, HLSL sees the same matrix and
//   mul(M, v) applies it. row_major packing would transpose them.

// --- The G-buffer ---

// A pixel of the G-buffer, decoded.
struct Material {
    float3 normal;     // shading normal in world space, normalized
    float roughness;   // perceptual: the game's GGX uses alpha = roughness^2, and NGX takes it as it is
    float3 base_color; // linear: the albedo is read through an _SRGB view
    float metallic;
};

static const float MIN_ROUGHNESS = 0.01f;

// NormalXRoughnessMisc.xy (RT2, R10G10B10A2_UNORM): the world normal in octahedral encoding, whose z
// is the world's Y.
inline float3 decode_normal(float2 encoded) {
    float3 n = float3(encoded.x * 2.0f - 1.0f, encoded.y * 2.0f - 1.0f, 0.0f);
    n.z = 1.0f - abs(n.x) - abs(n.y);

    // The usual fold of the lower hemisphere. The sign is never 0: a component at 0 counts as positive.
    if (n.z < 0.0f) {
        float x = (1.0f - abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        float y = (1.0f - abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = x;
        n.y = y;
    }

    return normalize(float3(n.x, n.z, n.y));
}

// NormalXRoughnessMisc.w is a 2-bit selector that the UNORM view reads as 0, 1/3, 2/3 or 1. Only 3
// makes BaseColorMetallic.w the metallic; with the others, the high bits of that alpha are a flag,
// probably of translucency.
inline float decode_metallic(float selector, float base_alpha) {
    return round(selector * 3.0f) == 3.0f ? base_alpha : 0.0f;
}

// normal_roughness_misc: RT2 of the G-buffer pass (R10G10B10A2_UNORM). base_color_metallic: RT1
// (R8G8B8A8_UNORM_SRGB).
inline Material decode_gbuffer(float4 normal_roughness_misc, float4 base_color_metallic) {
    Material m;
    m.normal = decode_normal(float2(normal_roughness_misc.x, normal_roughness_misc.y));
    m.roughness = max(MIN_ROUGHNESS, normal_roughness_misc.z);
    m.base_color = float3(base_color_metallic.x, base_color_metallic.y, base_color_metallic.z);
    m.metallic = decode_metallic(normal_roughness_misc.w, base_color_metallic.w);
    return m;
}

// --- The albedos ---

static const float DIELECTRIC_F0 = 0.04f;

inline float3 diffuse_albedo(Material m) {
    return m.base_color * (1.0f - m.metallic);
}

inline float3 specular_f0(Material m) {
    float3 f0 = m.base_color * m.metallic;
    return float3(max(DIELECTRIC_F0, f0.x), max(DIELECTRIC_F0, f0.y), max(DIELECTRIC_F0, f0.z));
}

// ambient_brdf: a sample of the game's AmbientBRDF LUT at ambient_brdf_uv, the scale in .x and the bias in .y.
inline float3 specular_albedo(float3 f0, float2 ambient_brdf) {
    return f0 * ambient_brdf.x + ambient_brdf.y;
}

// The GGX fit: the scale in .x and the bias in .y of the specular albedo, F0 * scale + bias, as the
// AmbientBRDF LUT holds them, at N.V and the perceptual roughness. The fallback of that LUT, and what the
// LUT is checked against, since neither depends on F0.
//
// The model is the preintegrated environment BRDF of GGX with alpha = roughness^2, height-correlated Smith
// masking and shadowing, and Schlick's Fresnel. Its integral, taken numerically over the domain, is fitted
// with a polynomial in N.V and the roughness: the lowest degree whose error, in floats, is at most 0.02 from
// N.V = 1/16 up and 0.05 below. With height-correlated masking the albedo goes back up to 1 at N.V = 0,
// within a band about roughness^2 wide that no polynomial of a reasonable degree follows.
inline float2 env_brdf_fit(float n_dot_v, float roughness) {
    float c = saturate(n_dot_v);
    float r = clamp(roughness, MIN_ROUGHNESS, 1.0f);

    // The coefficients of the fit, generated: do not edit them by hand.
    float scale = -51.7001038f;
    scale = scale * c + (202.899597f + r * (-20.0642757f));
    scale = scale * c + (-317.636047f + r * (30.3049698f + r * (35.8737526f)));
    scale = scale * c + (247.447739f + r * (61.429409f + r * (-211.546524f + r * (73.747963f))));
    scale = scale * c + (-94.0341797f + r * (-147.509521f + r * (306.08905f + r * (-93.8741531f + r * (-32.9966965f)))));
    scale = scale * c + (10.5395908f + r * (92.215271f + r * (-105.00576f + r * (-105.824623f + r * (156.728333f + r * (-34.783596f))))));
    scale = scale * c + (3.47276711f + r * (-17.6385117f + r * (-27.9562874f + r * (125.58223f + r * (-111.018326f + r * (18.8493633f
        + r * (5.78098059f)))))));
    scale = scale * c + (0.019345982f + r * (0.745549917f + r * (5.00832462f + r * (4.05377388f + r * (-49.4981155f + r * (80.4087677f
        + r * (-52.2084579f + r * (12.422781f))))))));
    float bias = 48.1976662f;
    bias = bias * c + (-188.03685f + r * (27.1735764f));
    bias = bias * c + (296.152863f + r * (-90.7701111f + r * (-0.625918746f)));
    bias = bias * c + (-235.494705f + r * (65.6800308f + r * (119.461632f + r * (-72.9338303f))));
    bias = bias * c + (93.0760269f + r * (50.4553185f + r * (-292.509064f + r * (209.77681f + r * (-25.7987366f)))));
    bias = bias * c + (-11.6109228f + r * (-73.502327f + r * (206.899231f + r * (-126.68618f + r * (-15.1898727f + r * (16.2499161f))))));
    bias = bias * c + (-3.24699116f + r * (21.9289436f + r * (-22.6686249f + r * (-47.875206f + r * (94.4926758f + r * (-52.0208817f
        + r * (9.38889599f)))))));
    bias = bias * c + (0.970232785f + r * (-1.17092264f + r * (-4.70548773f + r * (-4.58663654f + r * (73.5466614f + r * (-150.572083f
        + r * (123.984344f + r * (-37.4459724f))))))));
    // The end of the generated coefficients.

    return float2(max(0.0f, scale), max(0.0f, bias));
}

// --- The AmbientBRDF LUT ---

// Where the LUT is sampled: (saturate(N.V), roughness), as the PT's shader samples it, with the perceptual
// roughness on the V axis.
inline float2 ambient_brdf_uv(float n_dot_v, float roughness) {
    return float2(n_dot_v, roughness);
}

// The texels that SampleLevel at mip 0 reads with a linear filter and clamp addressing, as coordinates:
// the near ones, the far ones, which stay inside the LUT at its edges, and the weights of the far ones.
struct BilinearFootprint {
    float2 near_texel;
    float2 far_texel;
    float2 weights;
};

// size: the LUT's, in texels. Outside the outer texel centers, clamp addressing gives the edge's texel,
// which is what clamping the coordinate gives too.
inline BilinearFootprint bilinear_footprint(float2 uv, float2 size) {
    float x = clamp(uv.x * size.x - 0.5f, 0.0f, size.x - 1.0f);
    float y = clamp(uv.y * size.y - 0.5f, 0.0f, size.y - 1.0f);

    BilinearFootprint f;
    f.near_texel = float2(floor(x), floor(y));
    f.far_texel = float2(min(f.near_texel.x + 1.0f, size.x - 1.0f), min(f.near_texel.y + 1.0f, size.y - 1.0f));
    f.weights = float2(x - f.near_texel.x, y - f.near_texel.y);
    return f;
}

// The texels of a footprint, near and far in X for the near row, then for the far row.
inline float2 bilinear(float2 near_near, float2 far_near, float2 near_far, float2 far_far, float2 weights) {
    float2 near_row = near_near + (far_near - near_near) * weights.x;
    float2 far_row = near_far + (far_far - near_far) * weights.x;
    return near_row + (far_row - near_row) * weights.y;
}

// --- The guides of the G-buffer, and the sky ---

// What the pass writes to its three R16G16B16A16_FLOAT guides.
struct GuideTexels {
    float4 normal_roughness; // the roughness in .w: NGX's Packed roughness mode
    float3 diffuse;
    float3 specular;
};

// Reversed Z: the sky is where nothing was drawn, at depth 0.
inline bool is_sky(float depth) {
    return depth == 0.0f;
}

// specular: the specular albedo, from the LUT or from the GGX fit. The sky takes the values the RR guide
// asks for.
inline GuideTexels guide_texels(Material m, float3 specular, float depth) {
    GuideTexels t;

    if (is_sky(depth)) {
        t.normal_roughness = float4(0.0f, 0.0f, 0.0f, 0.0f);
        t.diffuse = float3(0.5f, 0.5f, 0.5f);
        t.specular = float3(0.0f, 0.0f, 0.0f);
    } else {
        t.normal_roughness = float4(m.normal, m.roughness);
        t.diffuse = diffuse_albedo(m);
        t.specular = specular;
    }

    return t;
}

// --- Positions ---

// pixel: the pixel center, (x + 0.5, y + 0.5).
inline float2 pixel_to_ndc(float2 pixel, float2 render_size) {
    return float2(pixel.x / render_size.x * 2.0f - 1.0f, 1.0f - pixel.y / render_size.y * 2.0f);
}

inline float3 from_homogeneous(float4 h) {
    return float3(h.x / h.w, h.y / h.w, h.z / h.w);
}

// inv_view_proj: Scene's, which carries the jitter of the depth, as the PT reconstructs its positions.
inline float3 world_position(float2 ndc, float depth, float4x4 inv_view_proj) {
    return from_homogeneous(mul(inv_view_proj, float4(ndc.x, ndc.y, depth, 1.0f)));
}

// V, for the N.V of the specular albedo. A perspective projection sends the camera to (0, 0, b, 0), so
// inv_view_proj * (0, 0, 1, 0) is the camera over b. The jitter does not move it.
inline float3 view_direction(float3 world, float4x4 inv_view_proj) {
    return normalize(from_homogeneous(mul(inv_view_proj, float4(0.0f, 0.0f, 1.0f, 0.0f))) - world);
}

// --- The MVs ---

// velocity: the game's (VelocityTarget.xy), or reproject_velocity's: ndc_prev - ndc_cur, which carries
// minus the current jitter. jitter: j, the sample's displacement in pixels this frame (X right, Y
// down), taken from the frame's matrices by the policy, never from the injected offset.
//
// Gives what NGX takes: render pixels towards the previous frame, with MVScale = 1 and without
// MVJittered. With the camera still, 0.
inline float2 motion_vector(float2 velocity, float2 jitter, float2 render_size) {
    return float2(velocity.x * render_size.x * 0.5f + jitter.x, -velocity.y * render_size.y * 0.5f + jitter.y);
}

// The game's velocity (R16G16B16A16_SNORM) saturates at 1 NDC, half the screen, when the real motion
// is larger. The SNORM view reads -32768 and -32767 as -1. Either channel saturated makes both wrong.
inline bool is_saturated(float2 velocity) {
    return abs(velocity.x) >= 1.0f || abs(velocity.y) >= 1.0f;
}

// How far reproject_velocity puts the previous position of a point that has none: two screen widths to
// the right, in NDC.
static const float OFFSCREEN_VELOCITY = 4.0f;

// The velocity of a saturated pixel, redone for a static point: world is the pixel's position and
// old_view_proj, the previous frame's view-projection without jitter. Gives ndc_prev - ndc_cur, in the
// game's convention, for motion_vector. The sky needs nothing special: the far plane is finite.
inline float2 reproject_velocity(float3 world, float2 ndc, float4x4 old_view_proj) {
    float4 prev = mul(old_view_proj, float4(world, 1.0f));

    // Behind the previous camera or on its plane (w <= 0), the point had no position on screen. No early
    // return: FXC takes it for an uninitialized result in cs_5_1.
    float2 v = float2(OFFSCREEN_VELOCITY, 0.0f);

    if (prev.w > 0.0f) {
        v = float2(prev.x / prev.w - ndc.x, prev.y / prev.w - ndc.y);
    }

    return v;
}

// The largest finite value of R16G16_FLOAT.
static const float MAX_MOTION_VECTOR_LENGTH = 65504.0f;

// The length of a limited MV: a little below the limit, so that the rounding of the division and the
// square root on the GPU cannot take it above.
static const float LIMITED_MOTION_VECTOR_LENGTH = 65500.0f;

// A bound on each channel against overflow, not a limit of the MV: up to it, the square of a channel
// stays finite, and an MV keeps its direction when limited.
static const float MOTION_VECTOR_CHANNEL_GUARD = 1e18f;

// Keeps the MV finite in R16G16_FLOAT: near the plane of the previous camera, the reprojection blows
// up. The length is limited, so the MV keeps its direction.
inline float2 limit_motion_vector(float2 mv) {
    // Per channel first, so that an infinite channel and the length stay finite.
    float2 m = float2(clamp(mv.x, -MOTION_VECTOR_CHANNEL_GUARD, MOTION_VECTOR_CHANNEL_GUARD),
        clamp(mv.y, -MOTION_VECTOR_CHANNEL_GUARD, MOTION_VECTOR_CHANNEL_GUARD));
    float len = length(m);

    if (len > MAX_MOTION_VECTOR_LENGTH) {
        m = m * (LIMITED_MOTION_VECTOR_LENGTH / len);
    }

    return m;
}

// --- The guide pass ---

// What the pass writes for one pixel, to its five textures.
struct GuidePixel {
    GuideTexels guides;
    float depth;          // plane 0 of the game's depth, as it is
    float2 motion_vector; // render pixels towards the previous frame
};

// What the pass reads of a pixel's surface, before its specular albedo, which needs a sample of the
// AmbientBRDF LUT at ambient_brdf_uv(n_dot_v, material.roughness).
struct Surface {
    Material material;
    float n_dot_v; // saturate(N.V): a normal facing away is grazing, as the LUT is sampled
    float depth;   // plane 0 of the game's depth, as it is
    float2 ndc;    // the pixel center's
    float3 world;  // the position that gives V, which the reprojection of the velocity reuses
};

// From what the pass's views read of the game's resources: normal_roughness_misc and base_color_metallic
// as decode_gbuffer takes them, and depth from plane 0. inv_view_proj comes from the camera update of the
// decision.
inline Surface surface_at(
    float2 pixel, float4 normal_roughness_misc, float4 base_color_metallic, float depth, float2 render_size, float4x4 inv_view_proj) {
    Surface s;
    s.material = decode_gbuffer(normal_roughness_misc, base_color_metallic);
    s.depth = depth;
    s.ndc = pixel_to_ndc(pixel, render_size);
    s.world = world_position(s.ndc, depth, inv_view_proj);
    s.n_dot_v = saturate(dot(s.material.normal, view_direction(s.world, inv_view_proj)));
    return s;
}

// The specular albedo: with has_lut, from ambient_brdf, a sample of the game's LUT at ambient_brdf_uv;
// without it, from the GGX fit at the same N.V and roughness, in the sample's place.
inline float3 surface_specular(Surface s, bool has_lut, float2 ambient_brdf) {
    float2 terms = env_brdf_fit(s.n_dot_v, s.material.roughness);

    if (has_lut) {
        terms = ambient_brdf;
    }

    return specular_albedo(specular_f0(s.material), terms);
}

// Where the pass takes the MVs from.
// The game's velocity everywhere, saturated or not: the reprojection off, as a switch for validation.
static const int MOTION_GAME_VELOCITY = 0;
// The game's velocity, but a saturated one is redone by reprojection, both channels.
static const int MOTION_REPROJECT_SATURATED = 1;
// A frame without velocity: every pixel by reprojection, and no velocity is read.
static const int MOTION_REPROJECT_ALL = 2;

// One pixel of the pass: its surface, its specular albedo as surface_specular gives it from has_lut and
// ambient_brdf, and velocity as motion_vector takes it. jitter and old_view_proj come from the camera
// update of the surface's inv_view_proj, as the policy gives them.
//
// motion is a MOTION_* above. The reprojection starts from the position that gives V, and gives moving
// objects the camera's motion only. With MOTION_REPROJECT_ALL, velocity is not used.
inline GuidePixel guide_pixel(Surface s, bool has_lut, float2 ambient_brdf, float2 velocity, float2 render_size, float2 jitter,
    float4x4 old_view_proj, int motion) {
    float2 v = velocity;

    if (motion == MOTION_REPROJECT_ALL || (motion == MOTION_REPROJECT_SATURATED && is_saturated(velocity))) {
        v = reproject_velocity(s.world, s.ndc, old_view_proj);
    }

    GuidePixel p;
    p.guides = guide_texels(s.material, surface_specular(s, has_lut, ambient_brdf), s.depth);
    p.depth = s.depth;
    // Within the limit, which the game's velocity never reaches, the MV stays as it is.
    p.motion_vector = limit_motion_vector(motion_vector(v, jitter, render_size));
    return p;
}

// --- The debug view ---

// The guide the debug view writes into O, in the order of the Mod's interface.
static const int DEBUG_VIEW_NORMALS = 0;
static const int DEBUG_VIEW_ROUGHNESS = 1;
static const int DEBUG_VIEW_DIFFUSE = 2;
static const int DEBUG_VIEW_SPECULAR = 3;
static const int DEBUG_VIEW_DEPTH = 4;
static const int DEBUG_VIEW_MOTION_VECTORS = 5;

// The largest finite value of the 10-bit channel of R11G11B10_FLOAT, O's format in RE2.
static const float MAX_DEBUG_VIEW_CHANNEL = 64512.0f;

// What the debug view writes into O for a texel of a guide's texture: normal_roughness for the normals
// and the roughness, the albedo for the albedos, the depth in .x and the MV in .xy. O comes before the
// exposure and the tonemapping, so scale brings the guide to a visible brightness; and O's channels have
// no sign, so they get nothing negative, infinite or NaN.
inline float3 debug_view_color(int guide, float4 texel, float scale) {
    float3 c = float3(texel.x, texel.y, texel.z);

    if (guide == DEBUG_VIEW_NORMALS) {
        // From 0 to 1 per axis, except the sky's normal 0, which would be grey.
        if (dot(c, c) > 0.0f) {
            c = c * 0.5f + 0.5f;
        } else {
            c = float3(0.0f, 0.0f, 0.0f);
        }
    } else if (guide == DEBUG_VIEW_ROUGHNESS) {
        c = float3(texel.w, texel.w, texel.w);
    } else if (guide == DEBUG_VIEW_DEPTH) {
        c = float3(texel.x, texel.x, texel.x);
    } else if (guide == DEBUG_VIEW_MOTION_VECTORS) {
        // Red to the right, blue to the left, and the vertical length in green.
        c = float3(max(texel.x, 0.0f), abs(texel.y), max(-texel.x, 0.0f));
    }

    c = c * scale;

    // min and max return the operand that is not NaN.
    return float3(clamp(c.x, 0.0f, MAX_DEBUG_VIEW_CHANNEL), clamp(c.y, 0.0f, MAX_DEBUG_VIEW_CHANNEL),
        clamp(c.z, 0.0f, MAX_DEBUG_VIEW_CHANNEL));
}

// --- The measurement interface ---

// The count of non-finite values in O. By the bits, so that no compiler can assume a float is never NaN:
// an exponent of all ones is infinite with a mantissa of 0, and NaN with any other.
static const int NONFINITE_NONE = 0;
static const int NONFINITE_NAN = 1;
static const int NONFINITE_INF = 2;

inline int channel_kind(float x) {
    uint magnitude = asuint(x) & 0x7fffffffu;
    return magnitude > 0x7f800000u ? NONFINITE_NAN : (magnitude == 0x7f800000u ? NONFINITE_INF : NONFINITE_NONE);
}

// A pixel with a NaN channel is NaN, whatever the others hold.
inline int nonfinite_kind(float3 c) {
    int x = channel_kind(c.x);
    int y = channel_kind(c.y);
    int z = channel_kind(c.z);
    int infinite = (x == NONFINITE_INF || y == NONFINITE_INF || z == NONFINITE_INF) ? NONFINITE_INF : NONFINITE_NONE;
    return (x == NONFINITE_NAN || y == NONFINITE_NAN || z == NONFINITE_NAN) ? NONFINITE_NAN : infinite;
}

// The accumulated reference, per pixel: the running mean of T in .xyz, and in .w the samples it holds.
// held: what the last frame left. With frames_before 0 the mean starts again, whatever held has: a mean of
// another size, or nothing yet. A sample with a NaN or an infinite channel stays out, or that pixel would
// hold it, and write it into O, for as long as the mean lasts. Until the pixel's first finite sample, it
// is black.
inline float4 accumulate_mean(float4 held, float3 frame, float frames_before) {
    float samples = frames_before > 0.0f ? held.w : 0.0f;
    float3 mean = float3(0.0f, 0.0f, 0.0f);

    if (samples > 0.0f) {
        mean = float3(held.x, held.y, held.z);
    }

    if (nonfinite_kind(frame) == NONFINITE_NONE) {
        samples = samples + 1.0f;
        mean = mean + (frame - mean) / samples;
    }

    return float4(mean.x, mean.y, mean.z, samples);
}

#endif // RR_GUIDES_HLSLI
