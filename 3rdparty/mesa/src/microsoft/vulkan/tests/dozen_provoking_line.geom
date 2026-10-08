#version 450
layout(triangles) in;
layout(line_strip,max_vertices=6) out;
layout(location=0) flat out uint number;
layout(location=1) flat out float floating;
layout(location=2) out float smooth_value;
void main() {
   for(uint strip=0;strip<2;strip++) {
      for(uint i=0;i<3;i++) {
         vec2 position=vec2(-0.75+float(i)*0.75,strip==0?-0.5:0.5);
         gl_Position=vec4(position,0.5,1);
         number=strip*3+i; floating=float(number)*0.25;
         smooth_value=(position.x+1.0)*0.5;
         EmitVertex();
      }
      EndPrimitive();
   }
}
