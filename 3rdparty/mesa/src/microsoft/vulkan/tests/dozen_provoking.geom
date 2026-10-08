#version 450
layout(triangles) in;
layout(triangle_strip,max_vertices=4) out;
layout(location=0) flat out uint number;
layout(location=1) flat out float floating;
layout(location=2) out float smooth_value;
void main() {
   vec2 positions[4]=vec2[4](vec2(-1,-1),vec2(-1,1),vec2(1,-1),vec2(1,1));
   for(uint i=0;i<4;i++) {
      gl_Position=vec4(positions[i],0.5,1);
      number=i; floating=float(i)*0.25;
      smooth_value=(positions[i].x+1.0)*0.5;
      EmitVertex();
   }
   // Exercise implicit end of the final strip.
}
