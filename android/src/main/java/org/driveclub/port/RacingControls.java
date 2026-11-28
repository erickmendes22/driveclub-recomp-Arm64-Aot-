package org.driveclub.port;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import java.util.ArrayList;
import java.util.List;

/** Touchscreen PS4 pad. Pointer IDs own controls until release, independently of event indices. */
public final class RacingControls extends View {
    public interface Listener { void onInput(State state); }
    public static final class State {
        public final int buttons;
        public final float lx, ly, rx, ry, l2, r2;
        public final boolean leftActive, rightActive;
        State(int buttons, float lx, float ly, float rx, float ry, float l2, float r2,
              boolean leftActive, boolean rightActive) {
            this.buttons=buttons; this.lx=lx; this.ly=ly; this.rx=rx; this.ry=ry;
            this.l2=l2; this.r2=r2; this.leftActive=leftActive; this.rightActive=rightActive;
        }
        public static State neutral() { return new State(0,0,0,0,0,0,0,false,false); }
    }
    private static final int LEFT=1, RIGHT=2, GAS=3, BRAKE=4, HANDBRAKE=5, CAMERA=6,
            PAUSE=7, MODE=8, UP=9, DOWN=10, DPAD_LEFT=11, DPAD_RIGHT=12,
            CROSS=13, CIRCLE=14, SQUARE=15, TRIANGLE=16, L1=17, R1=18, TOUCHPAD=19;
    private static final int RED=0xffef3340, WHITE=0xfff4f6fa, MUTED=0xffc6cbd3;
    private static final class Control {
        final int id, mask;
        final String glyph, label, description;
        final RectF bounds;
        final boolean round;
        Control(int id, int mask, String glyph, String label, String description, RectF bounds, boolean round) {
            this.id=id; this.mask=mask; this.glyph=glyph; this.label=label;
            this.description=description; this.bounds=bounds; this.round=round;
        }
    }
    private static final class Touch {
        final Control control;
        final float downY;
        float x, y, pedal=1;
        Touch(Control control, float x, float y) { this.control=control; this.x=x; this.y=y; downY=y; }
    }
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ArrayList<Control> controls=new ArrayList<>();
    private final SparseArray<Touch> touches=new SparseArray<>();
    private final AccessibilityManager accessibility;
    private final Nodes nodes=new Nodes();
    private Listener listener;
    private State state=State.neutral();
    private boolean menu;
    private float unit=1;
    private int generation, accessibilityFocus=View.NO_ID, hover=View.NO_ID;

    public RacingControls(Context context) {
        super(context);
        setFocusable(true);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setContentDescription("Controles de Driveclub na tela");
        accessibility=(AccessibilityManager)context.getSystemService(Context.ACCESSIBILITY_SERVICE);
    }
    public void setListener(Listener listener) { this.listener=listener; if (listener!=null) listener.onInput(state); }
    public State getState() { return state; }
    public boolean isMenuControlsVisible() { return menu; }
    public void setMenuControlsVisible(boolean visible) {
        if (menu==visible) return;
        releaseAll(); menu=visible; rebuild();
        sendAccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED);
        announceForAccessibility(menu ? "Botões de menu" : "Controles de corrida");
    }
    public void releaseAll() {
        ++generation; touches.clear(); state=State.neutral();
        if (getParent()!=null) getParent().requestDisallowInterceptTouchEvent(false);
        if (listener!=null) listener.onInput(state);
        invalidate();
    }
    @Override protected void onSizeChanged(int w,int h,int oldw,int oldh) {
        releaseAll(); rebuild();
    }
    @Override protected void onDetachedFromWindow() { releaseAll(); super.onDetachedFromWindow(); }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused); if (!focused) releaseAll();
    }
    private void add(int id,int mask,String glyph,String label,String description,
                     float x,float y,float w,float h,boolean round) {
        controls.add(new Control(id,mask,glyph,label,description,new RectF(x,y,x+w,y+h),round));
    }
    private void rebuild() {
        controls.clear(); accessibilityFocus=hover=View.NO_ID;
        float w=getWidth(),h=getHeight(); if (w<=0 || h<=0) return;
        unit=Math.min(getResources().getDisplayMetrics().density*1.2f,Math.min(w/740f,h/340f));
        float u=unit,m=16*u;
        add(MODE,0,menu?"CORRIDA":"MENU",menu?"Voltar à direção":"Botões do controle",
                menu?"Mostrar controles de corrida":"Mostrar botões de menu",w-m-100*u,m,100*u,42*u,false);
        add(PAUSE,0x8,"Ⅱ","Pausar","Pausar (Options)",w-m-192*u,m,82*u,42*u,false);
        add(LEFT,0,"",menu?"ANALÓGICO":"DIREÇÃO","Direção analógica; arraste para virar",
                m+10*u,h-m-154*u,136*u,136*u,true);
        if (!menu) {
            add(GAS,0x200,"R2","ACELERAR","Acelerar (R2); segure e deslize para baixo para dosar",
                    w-m-76*u,h-m-112*u,76*u,112*u,false);
            add(BRAKE,0x100,"L2","FREAR","Frear (L2); segure e deslize para baixo para dosar",
                    w-m-164*u,h-m-112*u,76*u,112*u,false);
            add(HANDBRAKE,0x2000,"○","FREIO DE MÃO","Freio de mão (Círculo)",
                    w-m-174*u,h-m-176*u,92*u,52*u,false);
            add(CAMERA,0x800,"R1","CÂMERA","Trocar câmera (R1)",
                    w-m-72*u,h-m-176*u,72*u,52*u,false);
            add(RIGHT,0,"","OLHAR","Olhar ao redor; analógico direito",
                    w-m-282*u,h-m-103*u,82*u,82*u,true);
        } else {
            float x=m+198*u,y=h-m-112*u,s=42*u,g=4*u;
            add(UP,0x10,"↑","","Direcional para cima",x,y-s-g,s,s,false);
            add(DOWN,0x40,"↓","","Direcional para baixo",x,y+s+g,s,s,false);
            add(DPAD_LEFT,0x80,"←","","Direcional esquerdo",x-s-g,y,s,s,false);
            add(DPAD_RIGHT,0x20,"→","","Direcional direito",x+s+g,y,s,s,false);
            float cx=w-m-94*u,cy=h-m-100*u,r=27*u,d=60*u;
            add(CROSS,0x4000,"×","CONFIRMAR","Confirmar (Cruz)",cx-r,cy+d-r,2*r,2*r,true);
            add(CIRCLE,0x2000,"○","VOLTAR","Voltar (Círculo)",cx+d-r,cy-r,2*r,2*r,true);
            add(SQUARE,0x8000,"□","","Quadrado",cx-d-r,cy-r,2*r,2*r,true);
            add(TRIANGLE,0x1000,"△","","Triângulo",cx-r,cy-d-r,2*r,2*r,true);
            add(L1,0x400,"L1","","L1",w/2-76*u,h-m-66*u,60*u,42*u,false);
            add(R1,0x800,"R1","","R1",w/2+16*u,h-m-66*u,60*u,42*u,false);
            add(TOUCHPAD,0x100000,"TOUCHPAD","","Pressionar touchpad",w/2-62*u,h-m-120*u,124*u,40*u,false);
        }
        invalidate();
    }
    private Control hit(float x,float y) {
        for (Control c:controls) {
            if (!c.bounds.contains(x,y)) continue;
            if (!c.round || Math.hypot(x-c.bounds.centerX(),y-c.bounds.centerY())<=c.bounds.width()/2) return c;
        }
        return null;
    }
    private boolean occupied(int control) {
        for(int i=0;i<touches.size();++i) if(touches.valueAt(i).control.id==control) return true;
        return false;
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action=event.getActionMasked(),index=event.getActionIndex();
        if (action==MotionEvent.ACTION_CANCEL) { releaseAll(); return true; }
        if (action==MotionEvent.ACTION_DOWN) {
            releaseAll();
            if (getParent()!=null) getParent().requestDisallowInterceptTouchEvent(true);
        }
        if (action==MotionEvent.ACTION_DOWN || action==MotionEvent.ACTION_POINTER_DOWN) {
            Control control=hit(event.getX(index),event.getY(index));
            if (control!=null && control.id==MODE) {
                setMenuControlsVisible(!menu); performClick(); return true;
            }
            if (control!=null && !((control.id==LEFT || control.id==RIGHT) && occupied(control.id)))
                touches.put(event.getPointerId(index),new Touch(control,event.getX(index),event.getY(index)));
        } else if (action==MotionEvent.ACTION_UP || action==MotionEvent.ACTION_POINTER_UP) {
            touches.remove(event.getPointerId(index));
            if (action==MotionEvent.ACTION_UP && getParent()!=null) getParent().requestDisallowInterceptTouchEvent(false);
        }
        for(int i=0;i<event.getPointerCount();++i) {
            Touch t=touches.get(event.getPointerId(i)); if(t==null) continue;
            t.x=event.getX(i); t.y=event.getY(i);
            if(t.control.id==GAS || t.control.id==BRAKE)
                t.pedal=clamp(1-(t.y-t.downY)/t.control.bounds.height(),0,1);
        }
        publish(); return true;
    }
    @Override public boolean performClick() { super.performClick(); return true; }
    private static float clamp(float v,float lo,float hi) { return Math.max(lo,Math.min(hi,v)); }
    private float[] stick(Touch t) {
        float travel=t.control.bounds.width()*.32f;
        float x=(t.x-t.control.bounds.centerX())/travel,y=(t.y-t.control.bounds.centerY())/travel;
        float length=(float)Math.hypot(x,y);
        if(length<.08f) return new float[]{0,0};
        float amount=(Math.min(length,1)-.08f)/.92f;
        return new float[]{x/length*amount,y/length*amount};
    }
    private void publish() {
        int buttons=0; float lx=0,ly=0,rx=0,ry=0,l2=0,r2=0; boolean left=false,right=false;
        for(int i=0;i<touches.size();++i) {
            Touch t=touches.valueAt(i); int id=t.control.id;
            if(id==LEFT || id==RIGHT) {
                float[] a=stick(t);
                if(id==LEFT) { lx=a[0];ly=a[1];left=true; } else { rx=a[0];ry=a[1];right=true; }
            } else if(id==GAS) { r2=Math.max(r2,t.pedal); if(t.pedal>.12f) buttons|=t.control.mask; }
            else if(id==BRAKE) { l2=Math.max(l2,t.pedal); if(t.pedal>.12f) buttons|=t.control.mask; }
            else buttons|=t.control.mask;
        }
        state=new State(buttons,lx,ly,rx,ry,l2,r2,left,right);
        if(listener!=null) listener.onInput(state);
        invalidate();
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        for(Control c:controls) {
            boolean active=occupied(c.id);
            if(c.id==LEFT || c.id==RIGHT) { drawStick(canvas,c,active);continue; }
            paint.setStyle(Paint.Style.FILL); paint.setColor(active?0xbbba2532:0x8c171c25);
            if(c.round) canvas.drawOval(c.bounds,paint); else canvas.drawRoundRect(c.bounds,12*unit,12*unit,paint);
            if(c.id==GAS || c.id==BRAKE) {
                float value=c.id==GAS?state.r2:state.l2;
                canvas.save();canvas.clipRect(c.bounds);
                paint.setColor(0x88ef3340);
                canvas.drawRect(c.bounds.left,c.bounds.bottom-value*c.bounds.height(),c.bounds.right,c.bounds.bottom,paint);
                canvas.restore();
            }
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth((active?2:1)*unit);
            paint.setColor(active?RED:0x99ffffff);
            if(c.round) canvas.drawOval(c.bounds,paint);else canvas.drawRoundRect(c.bounds,12*unit,12*unit,paint);
            float textY=c.bounds.centerY()+(c.label.isEmpty()?7:-1)*unit;
            label(canvas,c.glyph,c.bounds.centerX(),textY,c.id==MODE?13:22,WHITE,true);
            if(!c.label.isEmpty()) label(canvas,c.label,c.bounds.centerX(),textY+17*unit,c.id==MODE?9:9.5f,MUTED,true);
            if(c.id==accessibilityFocus) { paint.setColor(WHITE);canvas.drawRect(c.bounds,paint); }
        }
    }
    private void drawStick(Canvas canvas,Control c,boolean active) {
        float cx=c.bounds.centerX(),cy=c.bounds.centerY(),radius=c.bounds.width()/2;
        paint.setStyle(Paint.Style.FILL);paint.setColor(0x68121720);canvas.drawCircle(cx,cy,radius,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(unit);paint.setColor(active?RED:0x99ffffff);
        canvas.drawCircle(cx,cy,radius,paint);
        paint.setColor(0x44ffffff);canvas.drawCircle(cx,cy,radius*.66f,paint);
        canvas.drawLine(cx-radius*.32f,cy,cx+radius*.32f,cy,paint);
        canvas.drawLine(cx,cy-radius*.32f,cx,cy+radius*.32f,paint);
        float x=c.id==LEFT?state.lx:state.rx,y=c.id==LEFT?state.ly:state.ry;
        paint.setStyle(Paint.Style.FILL);paint.setColor(active?0xddf04450:0xaaaab2c0);
        canvas.drawCircle(cx+x*radius*.64f,cy+y*radius*.64f,radius*.32f,paint);
        label(canvas,c.label,cx,c.bounds.bottom+15*unit,c.id==LEFT?11:9,MUTED,true);
    }
    private void label(Canvas canvas,String text,float x,float y,float size,int color,boolean bold) {
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setTextSize(size*unit);
        paint.setTypeface(bold?Typeface.create("sans-serif-medium",Typeface.NORMAL):Typeface.DEFAULT);
        paint.setTextAlign(Paint.Align.CENTER);canvas.drawText(text,x,y,paint);
    }

    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return nodes; }
    private void event(int id,int type) {
        if(accessibility==null || !accessibility.isEnabled() || getParent()==null) return;
        AccessibilityEvent event=AccessibilityEvent.obtain(type);
        event.setPackageName(getContext().getPackageName());event.setSource(this,id);
        getParent().requestSendAccessibilityEvent(this,event);
    }
    @Override public boolean dispatchHoverEvent(MotionEvent e) {
        if(accessibility!=null && accessibility.isTouchExplorationEnabled()) {
            Control c=e.getActionMasked()==MotionEvent.ACTION_HOVER_EXIT?null:hit(e.getX(),e.getY());
            int next=c==null?View.NO_ID:c.id;
            if(next!=hover) { if(hover!=View.NO_ID) event(hover,AccessibilityEvent.TYPE_VIEW_HOVER_EXIT);
                hover=next;if(hover!=View.NO_ID) event(hover,AccessibilityEvent.TYPE_VIEW_HOVER_ENTER); }
            return next!=View.NO_ID;
        }
        return super.dispatchHoverEvent(e);
    }
    private final class Nodes extends AccessibilityNodeProvider {
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
            if(id==HOST_VIEW_ID) {
                AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain(RacingControls.this);
                onInitializeAccessibilityNodeInfo(node);
                for(Control c:controls) node.addChild(RacingControls.this,c.id);
                return node;
            }
            Control c=null;for(Control item:controls) if(item.id==id) c=item;
            if(c==null) return null;
            AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();
            node.setSource(RacingControls.this,id);node.setParent(RacingControls.this);
            node.setPackageName(getContext().getPackageName());node.setClassName("android.widget.Button");
            node.setContentDescription(c.description);node.setEnabled(true);node.setVisibleToUser(isShown());
            node.setFocusable(true);node.setClickable(true);node.setAccessibilityFocused(accessibilityFocus==id);
            Rect bounds=new Rect();c.bounds.roundOut(bounds);node.setBoundsInParent(bounds);
            int[] screen=new int[2];getLocationOnScreen(screen);bounds.offset(screen[0],screen[1]);node.setBoundsInScreen(bounds);
            node.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
            node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.AccessibilityAction.ACTION_CLEAR_ACCESSIBILITY_FOCUS:
                    AccessibilityNodeInfo.AccessibilityAction.ACTION_ACCESSIBILITY_FOCUS);
            if(id==LEFT || id==RIGHT) {
                node.setClassName("android.widget.SeekBar");
                node.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT,-1,1,id==LEFT?state.lx:state.rx));
                node.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS);
            }
            return node;
        }
        @Override public List<AccessibilityNodeInfo> findAccessibilityNodeInfosByText(String text,int id) {
            ArrayList<AccessibilityNodeInfo> found=new ArrayList<>();
            for(Control c:controls) if(c.description.toLowerCase(java.util.Locale.ROOT).contains(text.toLowerCase(java.util.Locale.ROOT)))
                found.add(createAccessibilityNodeInfo(c.id));
            return found;
        }
        @Override public boolean performAction(int id,int action,Bundle args) {
            Control c=null;for(Control item:controls) if(item.id==id) c=item;if(c==null) return false;
            if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS) {
                accessibilityFocus=id;event(id,AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED);invalidate();return true;
            }
            if(action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS) {
                accessibilityFocus=View.NO_ID;event(id,AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);invalidate();return true;
            }
            if(action==AccessibilityNodeInfo.ACTION_CLICK || action==AccessibilityNodeInfo.AccessibilityAction.ACTION_SET_PROGRESS.getId()) {
                if(id==MODE) { setMenuControlsVisible(!menu);return true; }
                float x=c.bounds.centerX(),y=c.bounds.centerY();
                if(args!=null && (id==LEFT || id==RIGHT))
                    x+=clamp(args.getFloat(AccessibilityNodeInfo.ACTION_ARGUMENT_PROGRESS_VALUE),-1,1)*c.bounds.width()*.32f;
                int pointer=-1000-id,epoch=generation;
                touches.put(pointer,new Touch(c,x,y));publish();event(id,AccessibilityEvent.TYPE_VIEW_CLICKED);
                postDelayed(()->{ if(generation==epoch) { touches.remove(pointer);publish(); } },150);
                return true;
            }
            return false;
        }
    }
}
