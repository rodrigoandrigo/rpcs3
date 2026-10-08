#version 450
layout(location=1) flat in uint first_vertex;
layout(location=0) out vec4 color;
void main() { color=first_vertex==100u?vec4(0,1,0,1):vec4(1,0,0,1); }
