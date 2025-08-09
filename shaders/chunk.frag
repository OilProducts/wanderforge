#version 450
layout(location=0) in vec3 vNormal;
layout(location=1) flat in uint vMat;
layout(location=0) out vec4 outColor;

vec3 matColor(uint m) {
  // Basic palette: rock, dirt, water, lava
  if (m == 1u) return vec3(0.6, 0.6, 0.6);     // rock
  if (m == 2u) return vec3(0.47, 0.28, 0.09);   // dirt
  if (m == 3u) return vec3(0.1, 0.4, 0.9);      // water
  if (m == 4u) return vec3(0.9, 0.3, 0.1);      // lava
  return vec3(0.82, 0.75, 0.66);                // default
}

void main() {
  vec3 N = normalize(vNormal);
  vec3 L = normalize(vec3(0.5, 0.8, 0.2));
  float diff = max(dot(N, L), 0.0);
  float ao = 0.2; // simple ambient
  vec3 base = matColor(vMat);
  vec3 col = base * (ao + (1.0 - ao) * diff);
  outColor = vec4(col, 1.0);
}
