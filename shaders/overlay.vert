#version 450
layout(location=0) in vec2 inPos;      // NDC coordinates [-1,1]
layout(location=1) in vec4 inColor;    // RGBA
layout(location=0) out vec4 vColor;
void main(){
  vColor = inColor;
  gl_Position = vec4(inPos, 0.0, 1.0);
}

