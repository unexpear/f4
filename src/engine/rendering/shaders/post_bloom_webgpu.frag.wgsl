// HDR bloom post chain: see post_bloom_vulkan.frag for the four modes.
struct PostUbo {
    params0: vec4f, // source texel size xy, threshold, knee
    params1: vec4f, // mode, intensity, 1 / target size xy
};

@group(0) @binding(0) var<uniform> post: PostUbo;
@group(0) @binding(1) var sourceTexture: texture_2d<f32>;
@group(0) @binding(2) var sourceSampler: sampler;
@group(0) @binding(5) var secondTexture: texture_2d<f32>;
@group(0) @binding(6) var secondSampler: sampler;

fn sample0(uv: vec2f) -> vec4f { return textureSampleLevel(sourceTexture, sourceSampler, uv, 0.0); }
fn sample1(uv: vec2f) -> vec4f { return textureSampleLevel(secondTexture, secondSampler, uv, 0.0); }

fn linearToSrgb(value: vec3f) -> vec3f {
    let c = max(value, vec3f(0.0));
    let low = c * 12.92;
    let high = 1.055 * pow(c, vec3f(1.0 / 2.4)) - vec3f(0.055);
    return select(high, low, c <= vec3f(0.0031308));
}

fn acesToneMap(color: vec3f) -> vec3f {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((color * (a * color + vec3f(b))) /
                 (color * (c * color + vec3f(d)) + vec3f(e)), vec3f(0.0), vec3f(1.0));
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let uv = position.xy * post.params1.zw;
    let mode = i32(post.params1.x + 0.5);
    let t = post.params0.xy;
    if (mode <= 1) {
        var c = (sample0(uv + vec2f(-t.x, -t.y)) + sample0(uv + vec2f(t.x, -t.y)) +
                 sample0(uv + vec2f(-t.x, t.y)) + sample0(uv + t)).rgb * 0.25;
        if (mode == 0) {
            c = min(c, vec3f(64.0));
            let brightness = max(c.r, max(c.g, c.b));
            let knee = post.params0.w;
            var soft = clamp(brightness - post.params0.z + knee, 0.0, 2.0 * knee);
            soft = soft * soft / (4.0 * knee + 1.0e-5);
            c *= max(soft, brightness - post.params0.z) / max(brightness, 1.0e-5);
        }
        return vec4f(c, 1.0);
    }
    if (mode == 2) {
        var s = sample0(uv + vec2f(-t.x, -t.y)).rgb + sample0(uv + vec2f(t.x, -t.y)).rgb +
                sample0(uv + vec2f(-t.x, t.y)).rgb + sample0(uv + t).rgb;
        s += 2.0 * (sample0(uv + vec2f(0.0, -t.y)).rgb + sample0(uv + vec2f(-t.x, 0.0)).rgb +
                    sample0(uv + vec2f(t.x, 0.0)).rgb + sample0(uv + vec2f(0.0, t.y)).rgb);
        s += 4.0 * sample0(uv).rgb;
        return vec4f(sample1(uv).rgb + s / 16.0, 1.0);
    }
    let scene = sample0(uv);
    let color = scene.rgb + sample1(uv).rgb * post.params1.y;
    return vec4f(linearToSrgb(acesToneMap(color)), scene.a);
}
