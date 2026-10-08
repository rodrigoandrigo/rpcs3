#version 450
layout(location=0) in vec2 position;
layout(location=0) flat out uint number;
layout(location=1) flat out float floating;
layout(location=2) out float smooth_value;
void main() {
   gl_Position=vec4(position,0.5,1);
   number=uint(gl_VertexIndex);
   floating=float(number)*0.25;
   smooth_value=(position.x+1.0)*0.5;
}
