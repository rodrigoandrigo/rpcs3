#version 450
layout(location=0) flat in uint number;
layout(location=1) flat in float floating;
layout(location=2) in float smooth_value;
layout(location=0) out vec4 color;
void main() {
   color=vec4(float(number)/255.0,
              floating==float(number)*0.25 ? 1.0 : 0.0,
              abs(smooth_value-gl_FragCoord.x/32.0)<0.001 ? 1.0 : 0.0,1);
}
