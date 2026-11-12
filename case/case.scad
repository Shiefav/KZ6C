// KZ6C-B02 preliminary mechanical fit; mm. Origin PCB centre, +Y toward USB.
// Component envelope / assembly verification and real parts still mandatory.
$fn=96;
mode="assembly"; // top, spacer, bottom, assembly
xs=[-19.05,0,19.05];
module perimeter(){ offset(r=3) square([58,58],center=true); }
module holes(){ for(x=[-28.5,28.5],y=[-28.5,28.5]) translate([x,y]) circle(d=2.2); }
module top(){ difference(){ perimeter();holes();
 for(x=xs) translate([x,7]) square([14,14],center=true);
 // Body is nominally 6x6 mm. A full opening avoids dependence on its height.
 for(x=xs) translate([x,-19]) square([6.8,6.8],center=true);
} }
module bottom(){ difference(){perimeter();holes();} }
module spacer(){ difference(){
 union(){
  difference(){perimeter(); square([58,58],center=true);}
  for(x=[-28.5,28.5],y=[-28.5,28.5]) translate([x,y]) circle(r=2.8);
 }
 holes();
 // Includes USB, CC resistors and exposed test pads at the top edge.
 translate([0,30]) square([44,10],center=true);
} }
module assembly(){
 color([.08,.08,.09]) translate([0,0,3.5]) linear_extrude(1.5) top();
 color([.02,.35,.12]) translate([0,0,-1.6]) linear_extrude(1.6) bottom();
 color([.65,.85,1,.45]) translate([0,0,-5.6]) linear_extrude(4) spacer();
 color([.08,.08,.09]) translate([0,0,-7.1]) linear_extrude(1.5) bottom();
 for(x=[-28.5,28.5],y=[-28.5,28.5]) color([.7,.7,.7]) translate([x,y,0]) linear_extrude(3.5) difference(){circle(d=4.5);circle(d=2.2);}
 for(x=xs){
  color([.12,.12,.12]) translate([x,7,5]) linear_extrude(8) square([18,18],center=true);
  color([.4,.4,.4]) translate([x,-19,0]) linear_extrude(3.5) square([6,6],center=true);
  color([.1,.1,.1]) translate([x,-19,3.5]) cylinder(h=4.5,d=3.5);
 }
 color([.7,.7,.72]) translate([0,29.5,-4.7]) cube([9,7,3.1],center=true);
}
if(mode=="top") top(); else if(mode=="spacer") spacer(); else if(mode=="bottom") bottom(); else assembly();
