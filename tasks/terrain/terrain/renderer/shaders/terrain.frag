#version 450
layout(location = 0) in VS_OUT { vec3 wPos; } surf;
layout(location = 0) out vec4 out_fragColor;
void main() {
  vec3 dx = dFdx(surf.wPos);
  vec3 dy = dFdy(surf.wPos);
  vec3 normal = normalize(cross(dx, dy));
  if (normal.y < 0.0) normal = -normal;
  vec3 lightDir = normalize(vec3(1.0, 0.8, 0.5));
  float diff = max(dot(normal, lightDir), 0.0);
  float ambient = 0.3;
  float h = clamp(surf.wPos.y / 30.0, 0.0, 1.0);
  vec3 color = mix(vec3(0.2, 0.5, 0.1), vec3(0.9, 0.9, 0.9), h);
  out_fragColor = vec4(color * (diff + ambient), 1.0);
}