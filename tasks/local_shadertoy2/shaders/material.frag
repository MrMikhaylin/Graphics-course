#version 430

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec2 iResolution;
    float iTime;
    vec4 iMouse;
} pc;

float hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash(i + vec3(0,0,0));
    float n100 = hash(i + vec3(1,0,0));
    float n010 = hash(i + vec3(0,1,0));
    float n110 = hash(i + vec3(1,1,0));
    float n001 = hash(i + vec3(0,0,1));
    float n101 = hash(i + vec3(1,0,1));
    float n011 = hash(i + vec3(0,1,1));
    float n111 = hash(i + vec3(1,1,1));

    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);

    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);

    return mix(nxy0, nxy1, f.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

vec3 materialTexture(vec2 uv, float time) {
    float u = uv.x * 6.0;
    float v = uv.y * 3.0;

    float turb = fbm(vec3(u * 2.0, v * 2.0, time * 0.1));

    float bands = sin(v * 12.0 + turb * 6.0) * 0.5 + 0.5;
    bands = smoothstep(0.2, 0.8, bands);

    float bands2 = sin(v * 24.0 + turb * 10.0) * 0.5 + 0.5;
    bands2 = smoothstep(0.4, 0.6, bands2);

    float grain = fbm(vec3(u * 16.0, v * 16.0, time * 0.05));

    float rough = mix(0.15, 0.85, bands);
    rough = mix(rough, 0.35, bands2 * 0.5);
    rough += (grain - 0.5) * 0.15;
    rough = clamp(rough, 0.05, 1.0);

    float metal = bands2 * 0.35 + (grain - 0.5) * 0.1;
    metal = clamp(metal, 0.0, 1.0);

    float ao = mix(0.7, 1.0, bands);

    return vec3(rough, metal, ao);
}

void main() {
    vec2 uv = gl_FragCoord.xy / pc.iResolution.xy;
    vec3 mat = materialTexture(uv, pc.iTime);
    outColor = vec4(mat, 1.0);
}