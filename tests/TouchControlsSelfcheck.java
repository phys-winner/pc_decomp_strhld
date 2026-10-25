package org.stronghold.port;

import java.util.ArrayList;

public final class TouchControlsSelfcheck {
    static final ArrayList<String> events = new ArrayList<>();
    static float cursorX, cursorY;
    static TouchControls controls = new TouchControls(new TouchControls.Output() {
        public void move(float x, float y, boolean warp) { cursorX = x; cursorY = y; }
        public void cursor(float x, float y) { cursorX = x; cursorY = y; }
        public void button(int b, boolean down) { events.add("button:" + b + ":" + down); }
        public void click(int b) { events.add("click:" + b); }
        public void key(int scan, boolean down) { events.add("key:" + scan + ":" + down); }
    }, 8);
    static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message + ": " + events); }
    static void reset(boolean pad) { controls.cancel(); controls.bounds(0, 0, 1000, 600); controls.mode(pad, 1); events.clear(); }
    public static void main(String[] args) {
        reset(false); controls.down(100, 100, 0); controls.move(103, 102);
        check(events.isEmpty(), "selection must wait for movement");
        controls.up(103, 102, 100); check(events.size() == 1 && events.contains("click:0"), "tap");
        reset(false); controls.down(100, 100, 0); controls.move(130, 140); controls.cancel();
        check(events.toString().equals("[button:0:true, button:0:false]"), "drag cancelled releases LMB");
        reset(false); controls.down(100, 100, 0); controls.secondDown(100, 100, 150, 100, 20);
        controls.pointerUp(100); controls.move(200, 200); controls.up(200, 200, 120);
        check(events.toString().equals("[click:1]"), "two-finger tap never leaks LMB after either pointer lifts");
        reset(false); controls.down(100, 100, 0); controls.move(140, 100);
        controls.secondDown(140, 100, 180, 100, 30); controls.pairMove(180, 100, 220, 100); controls.pointerUp(150); controls.up(180, 100, 160);
        check(events.toString().equals("[button:0:true, button:0:false, key:203:true, key:203:false]"), "pan releases selection and all arrows");
        reset(false); controls.down(100, 100, 0); controls.secondDown(100, 100, 200, 100, 10);
        controls.pairMove(50, 100, 250, 100); controls.pointerUp(200); controls.up(250, 100, 220);
        check(events.isEmpty(), "pinch cannot become cancel tap");
        reset(false); controls.down(100, 100, 0); controls.longPress(500); controls.up(100, 100, 600);
        check(events.toString().equals("[click:1]"), "long press only RMB");
        reset(false); controls.down(100, 100, 0); controls.cancel(); controls.up(100, 100, 100);
        check(events.isEmpty(), "cancel cannot click");
        reset(true); controls.down(100, 100, 0); controls.move(10000, -10000); controls.up(10000, -10000, 100);
        check(cursorX == 1000 && cursorY == 0 && events.isEmpty(), "touchpad moves/clamps without dragging");
        int[] wide = TouchControls.worldResolution(2340, 1080, true);
        int[] fit = TouchControls.worldResolution(2340, 1080, false);
        check(wide[0] == 2344 && wide[1] == 1080, "ultrawide requested");
        check(fit[0] == 1920 && fit[1] == 1080, "16:9 remains proportional");
        int[] tablet = TouchControls.worldResolution(2944, 1840, true);
        check(tablet[0] == 2944 && tablet[1] == 1840, "tablet uses device pixels");
        int[] large = TouchControls.worldResolution(7680, 4320, true);
        check(large[0] <= 4056 && large[1] <= 2076, "fixed guest raster capacity");
        int[] capped = TouchControls.worldResolution(3600,901,true);
        check(capped[0] % 8 == 0 && capped[0] <= capped[1] * 3, "alignment cannot exceed the native aspect limit");
        int[] half = TouchControls.resolution("auto",2944,1840,2,true);
        check(half[0]==1472 && half[1]==920,"high DPI automatic half resolution");
        int[] preset = TouchControls.resolution("1920x1080",2340,1080,3,true);
        check(preset[0]==2344 && preset[1]==1080,"device aspect applies to presets");
        preset=TouchControls.resolution("1920x1080",2340,1080,3,false);
        check(preset[0]==1920 && preset[1]==1080,"preset 16:9 without device aspect");
        check(TouchControls.resolution("auto",2340,1080,1,true)[1]==1080,"low DPI remains native");
        check(TouchControls.resolution("auto",2340,1080,3,true)[1]==600,"original minimum resolution");
        int[] minimum=TouchControls.resolution("minimum",2944,1840,2,true);
        check(minimum[0]==960&&minimum[1]==600,"minimum keeps the engine height and device aspect");
        minimum=TouchControls.resolution("minimum",2340,1080,3,true);
        check(minimum[0]==1304&&minimum[1]==600,"minimum on an ultrawide phone");
        minimum=TouchControls.resolution("minimum",2340,1080,3,false);
        check(minimum[0]==1072&&minimum[1]==600,"minimum 16:9 without device aspect");
        for(int i=0;i<9;i++) {
            double a=i*Math.PI*2/9-Math.PI/2;
            check(TouchControls.radialSector((float)Math.cos(a)*100,(float)Math.sin(a)*100,0,0,100)==i,"stable wheel sector");
        }
        check(TouchControls.radialSector(0,0,0,0,100)==-1 && TouchControls.radialSector(200,0,0,0,100)==-1,"wheel center and outside cancel");
        reset(true); controls.place(5000, -20); check(cursorX == 1000 && cursorY == 0 && events.isEmpty(), "touchpad cursor continues from a mouse, clamped");
        autoPlus();
        System.out.println("TouchControls selection, gestures, cancellation, touchpad, resolution and Auto+ levels: PASS");
    }
    static void autoPlus() {
        /* The ladder: from the engine minimum up to the device's own height, starting at Авто. */
        int[] levels=TouchControls.autoPlusLevels(2340,1080,3);
        check(java.util.Arrays.equals(levels,new int[]{600,720,864,1080}),"Авто+ ladder of this phone");
        check(TouchControls.autoPlusStart(levels,2340,1080,3)==0,"Авто+ starts from the Авто size");
        int[] tablet=TouchControls.autoPlusLevels(2944,1840,2);
        check(tablet[0]==600&&tablet[tablet.length-1]==1840&&tablet[TouchControls.autoPlusStart(tablet,2944,1840,2)]==920,
                "a tablet starts at its Авто 920 and may go up to 1840 or down to 600");
        int[] low=TouchControls.autoPlusLevels(1600,720,1.5f);
        check(java.util.Arrays.equals(low,new int[]{600,720})&&TouchControls.autoPlusStart(low,1600,720,1.5f)==1,"low DPI starts native, may only step down");
        int[] huge=TouchControls.autoPlusLevels(7680,4320,3);
        check(huge[huge.length-1]==2076,"capped at the engine's tallest frame");
        for(int i=1;i<huge.length;i++)check(huge[i]>huge[i-1],"ascending levels");
        /* Sizes of the levels keep the width alignment and aspect of the fixed modes. */
        check(java.util.Arrays.equals(TouchControls.resolution("autoplus",2340,1080,3,true),TouchControls.resolution("auto",2340,1080,3,true)),"Авто+ base is Авто");
        int[] level=TouchControls.resolution("x720",2340,1080,3,true);
        check(level[0]==1560&&level[1]==720,"720 lines with the device aspect");
        level=TouchControls.resolution("x864",2340,1080,3,false);
        check(level[0]==1536&&level[1]==864&&level[0]%8==0,"864 lines at 16:9");
        /* Steps: comfortable over two minutes goes up; over the margin goes down. */
        float[] p95={12,0,0,0};int[] frames={4800,0,0,0};
        check(TouchControls.autoPlusStep(0,p95,frames)==1,"comfortable p95 over two minutes of match frames steps up");
        frames[0]=4799;check(TouchControls.autoPlusStep(0,p95,frames)==0,"not before two minutes of frames");
        frames[0]=4800;p95[0]=15;check(TouchControls.autoPlusStep(0,p95,frames)==0,"15 ms is not comfortably below budget");
        p95=new float[]{12,24,0,0};frames=new int[]{9000,1200,0,0};
        check(TouchControls.autoPlusStep(1,p95,frames)==0,"a level over 23 ms steps down");
        check(TouchControls.autoPlusStep(0,p95,frames)==0,"a level measured too slow is not re-entered");
        frames[1]=1199;check(TouchControls.autoPlusStep(1,p95,frames)==1,"a short slow spell is no evidence");
        check(TouchControls.autoPlusStep(0,p95,frames)==1,"so the upper level is still open");
        frames[1]=6000;p95[1]=20;
        check(TouchControls.autoPlusStep(1,p95,frames)==1&&TouchControls.autoPlusStep(0,p95,frames)==1,"between 15 and 23 ms a level holds and stays reachable");
        p95=new float[]{30,0,0,10};frames=new int[]{9000,0,0,9000};
        check(TouchControls.autoPlusStep(0,p95,frames)==0&&TouchControls.autoPlusStep(3,p95,frames)==3,"the ladder's ends hold");
        check(TouchControls.autoPlusStep(7,p95,frames)==0&&TouchControls.autoPlusStep(-1,p95,frames)==0,"an unknown level restarts at the bottom");
        /* Records: frame-weighted, slowly forgetting, ignoring empty samples. */
        float[] record=TouchControls.autoPlusRecord(0,0,12,240);
        check(record[0]==12&&record[1]==240,"first sample");
        record=TouchControls.autoPlusRecord(record[0],(int)record[1],24,240);
        check(Math.abs(record[0]-18)<1e-4&&record[1]==480,"frame-weighted mean");
        record=TouchControls.autoPlusRecord(10,1000000,30,24000);
        check(Math.abs(record[0]-20)<1e-4&&record[1]==1024000,"old history weighs at most ten minutes");
        record=TouchControls.autoPlusRecord(10,100,Float.NaN,240);
        check(record[0]==10&&record[1]==100,"a broken sample changes nothing");
    }
}
