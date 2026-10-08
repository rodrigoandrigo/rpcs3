#version 450
#extension GL_ARB_enhanced_layouts : require
layout(location=0) in vec2 position;
layout(location=0, xfb_buffer=0, xfb_offset=8, xfb_stride=16) out float captured;
layout(location=1) flat out uint first_vertex;
void main() {
   gl_Position=vec4(position,0.5,1.0);
   captured=float(gl_VertexIndex+100);
   first_vertex=uint(gl_VertexIndex+100);
}
