#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=2) in uint inMat;

layout(location=0) out vec3 vNormal;
layout(location=1) flat out uint vMat;

layout(push_constant) uniform PC {
  mat4 mvp;
} pc;

void main() {
  vNormal = inNormal;
  vMat = inMat;
  gl_Position = pc.mvp * vec4(inPos, 1.0);
}
