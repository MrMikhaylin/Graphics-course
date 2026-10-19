#version 430

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D albedoSampler;
layout(binding = 1) uniform sampler2D materialSampler;
layout(binding = 2) uniform sampler2D loadedSampler;

layout(push_constant) uniform PushConstants {
    vec2 iResolution;
    float iTime;
    vec4 iMouse;
} pc;

#define MAX_STEPS 100
#define MAX_DIST  50.0
#define EPS       0.001
#define PI        3.14159265359

float sdTorus(vec3 p, float R, float r) {
    vec2 q = vec2(length(p.xz) - R, p.y);
    return length(q) - r;
}

float sdPlane(vec3 p, float h) {
    return p.y - h;
}

float map(vec3 p) {
    float torus = sdTorus(p - vec3(0.0, 1.0, 0.0), 1.0, 0.35);
    float plane = sdPlane(p, -0.5);
    return min(torus, plane);
}

vec3 calcNormal(vec3 p) {
    vec2 e = vec2(0.001, 0.0);
    return normalize(vec3(
        map(p + e.xyy) - map(p - e.xyy),
        map(p + e.yxy) - map(p - e.yxy),
        map(p + e.yyx) - map(p - e.yyx)
    ));
}

float raymarch(vec3 ro, vec3 rd) {
    float t = 0.0;
    for (int i = 0; i < MAX_STEPS; i++) {
        vec3 p = ro + rd * t;
        float d = map(p);
        if (d < EPS) return t;
        t += d;
        if (t > MAX_DIST) break;
    }
    return -1.0;
}

float shadow(vec3 p, vec3 lightDir) {
    float t = 0.05;
    for (int i = 0; i < 32; i++) {
        float d = map(p + lightDir * t);
        if (d < 0.001) return 0.2;
        t += d;
        if (t > 10.0) break;
    }
    return 1.0;
}

vec2 torusUV(vec3 p) {
    vec3 pl = p - vec3(0.0, 1.0, 0.0);
    float R = 1.0;
    float u = atan(pl.z, pl.x) / (2.0 * PI) + 0.5;
    float v = atan(pl.y, length(pl.xz) - R) / (2.0 * PI) + 0.5;
    return vec2(u, v);
}

float D_GGX(float NdotH, float rough) {
    float a  = rough * rough;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-4);
}

float G_SchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float rough) {
    return G_SchlickGGX(NdotV, rough) * G_SchlickGGX(NdotL, rough);
}

vec3 F_Schlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float rough) {
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 pbrShade(vec3 albedo, float rough, float metal, float ao,
              vec3 N, vec3 V, vec3 L, vec3 lightCol) {
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);

    float D = D_GGX(NdotH, rough);
    float G = G_Smith(NdotV, NdotL, rough);
    vec3  F = F_Schlick(VdotH, F0);

    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);

    vec3 kD = (1.0 - F) * (1.0 - metal);

    vec3 direct = (kD * albedo / PI + spec) * lightCol * NdotL;

    vec3 F_amb = fresnelSchlickRoughness(NdotV, F0, rough);
    vec3 kD_amb = (1.0 - F_amb) * (1.0 - metal);
    vec3 ambient = kD_amb * albedo * 0.3 * ao;

    return direct + ambient;
}

void main() {
    vec2 fragCoord = vec2(gl_FragCoord.x, pc.iResolution.y - gl_FragCoord.y);

    vec2 uv = (fragCoord - 0.5 * pc.iResolution.xy) / pc.iResolution.y;

    vec2 mouse = (pc.iMouse.xy - 0.5 * pc.iResolution.xy) / pc.iResolution.y;
    if (pc.iMouse.z <= 0.0) mouse = vec2(0.0, 0.2);

    float yaw   = mouse.x * 3.0;
    float pitch = clamp(mouse.y * 1.2, -1.0, 1.0);

    float r = 5.0;
    vec3 ro = vec3(r * cos(pitch) * sin(yaw),
                   r * sin(pitch) + 1.0,
                   r * cos(pitch) * cos(yaw));
    vec3 ta = vec3(0.0, 1.0, 0.0);
    vec3 fw = normalize(ta - ro);
    vec3 rt = normalize(cross(vec3(0.0, 1.0, 0.0), fw));
    vec3 up = cross(fw, rt);
    vec3 rd = normalize(uv.x * rt + uv.y * up + 1.5 * fw);

    vec3 col = mix(vec3(0.5, 0.7, 1.0), vec3(0.1, 0.15, 0.3), uv.y * 0.5 + 0.5);

    float t = raymarch(ro, rd);
    if (t > 0.0) {
        vec3 p = ro + rd * t;
        vec3 n = calcNormal(p);
        vec3 V = -rd;

        vec3 lightDir = normalize(vec3(0.8, 1.0, 0.5));
        vec3 lightCol = vec3(1.0, 0.95, 0.85) * 2.5;

        float isFloor = step(p.y, -0.49);

        vec3 albedo;
        float rough, metal, ao;

        if (isFloor > 0.5) {
            vec2 floorUV = p.xz * 0.25;
            albedo = texture(loadedSampler, floorUV).rgb;
            rough  = 0.7;
            metal  = 0.0;
            ao     = 1.0;
        } else {
            vec2 uvT = torusUV(p);

            vec3 texAlb = texture(albedoSampler, uvT).rgb;
            vec3 texMat = texture(materialSampler, uvT).rgb;

            albedo = texAlb;
            rough  = texMat.r;
            metal  = texMat.g;
            ao     = texMat.b;
        }

        float sh = shadow(p + n * 0.01, lightDir);

        col = pbrShade(albedo, rough, metal, ao, n, V, lightDir, lightCol * sh);
    }

    col = col * 0.85;
    col = pow(col, vec3(0.4545));

    outColor = vec4(col, 1.0);
}