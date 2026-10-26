#version 450
layout(location = 0) in vec3 inPos;
layout(push_constant) uniform params_t {
  mat4 mProjView;
  vec3 camPos;
  float terrainHeight;
} params;
layout(location = 0) out VS_OUT { vec3 wPos; } vOut;
void main() {
  vOut.wPos = inPos;
  gl_Position = params.mProjView * vec4(inPos, 1.0);
}