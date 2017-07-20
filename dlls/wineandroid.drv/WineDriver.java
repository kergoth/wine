/*
 * WineDriver class
 *
 * Copyright 2013 Alexandre Julliard
 */

package org.winehq.wine;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.ClipboardManager;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ContentProvider;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.Rect;
import android.graphics.SurfaceTexture;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.TextView;
import android.widget.Toast;
import java.lang.ClassNotFoundException;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PipedInputStream;
import java.io.PipedOutputStream;
import java.nio.charset.Charset;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedList;
import java.util.Map;
import java.util.Map.Entry;

import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.CorrectionInfo;
import android.text.Spannable;
import android.text.Editable;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.ExtractedText;
import android.text.InputType;
import android.view.KeyCharacterMap;

public class WineDriver extends Object
{
    private native String wine_init( String[] cmdline, String[] env );

    private Activity activity;
    private ProgressDialog progDlg;
    private TextView progressStatusText;
    private int startupDpi;
    private boolean firstRun;

    private String providerAuthority;
    private static WineDriver clipboard_driver;

    public WineDriver( Activity act )
    {
        activity = act;
        clipboard_driver = this;
    }

    private Activity getActivity()
    {
        return activity;
    }

    private void runOnUiThread( Runnable r )
    {
        activity.runOnUiThread( r );
    }

    private final void runWine( String libwine, HashMap<String,String> wineEnv, String[] cmdline )
    {
        System.load( libwine );

        File prefix = new File( wineEnv.get( "WINEPREFIX" ) );
        prefix.mkdirs();

        String[] env = new String[wineEnv.size() * 2];
        int j = 0;
        for (Map.Entry<String,String> entry : wineEnv.entrySet())
        {
            env[j++] = entry.getKey();
            env[j++] = entry.getValue();
        }

        runOnUiThread( new Runnable() { public void run() { initProgDlg(); } } );

        String[] cmd = new String[3 + cmdline.length];
        cmd[0] = wineEnv.get( "WINELOADER" );
        cmd[1] = "explorer.exe";
        cmd[2] = "/desktop=" + get_desktop_name() + ",,android";
        System.arraycopy( cmdline, 0, cmd, 3, cmdline.length );

        String cmd_str = "";
        for (String s : cmd) cmd_str += " " + s;
        Log.i( "wine", "Running startup program:" + cmd_str );

        String err = wine_init( cmd, env );
        Log.e( "wine", err );
    }

    private void initProgDlg()
    {
        progDlg = new ProgressDialog( activity );
        progDlg.setTitle( "Initialization in progress" );
        progDlg.setMessage( "Setting up virtual Windows environment" );
        progDlg.setMax( 1 );
        progDlg.show();
    }

    public void setStartupDpi( int dpi )
    {
        startupDpi = dpi;
    }

    public void loadWine( final String libwine, final HashMap<String,String> wineEnv, final String[] cmdline )
    {
        firstRun = !(new File ( wineEnv.get( "WINEPREFIX" ) ).exists());
        String existing = firstRun ? "new" : "existing";
        Log.i( "wine", "Initializing wine in " + existing + " prefix " + wineEnv.get( "WINEPREFIX" ) );
        Runnable main_thread = new Runnable() { public void run() { runWine( libwine, wineEnv, cmdline ); } };
        new Thread( main_thread ).start();
    }

    public void runCmdline( final String cmdline, HashMap<String,String> envMap )
    {
        String[] env = null;
        Log.i( "wine", "Running command line: " + cmdline );
        if (cmdline == null) return;

        if (envMap != null)
        {
            int j = 0;
            env = new String[envMap.size() * 2];
            for (Map.Entry<String,String> entry : envMap.entrySet())
            {
                env[j++] = entry.getKey();
                env[j++] = entry.getValue();
            }
        }

        wine_run_commandline( cmdline, env );
    }

    public void setProviderAuthority( final String authority )
    {
        this.providerAuthority = authority;
    }

    protected String get_desktop_name()
    {
        return "shell";
    }

    public native void wine_send_gamepad_count(int count);
    public native void wine_send_gamepad_data(int index, int id, String name);
    public native void wine_send_gamepad_axis(int device, float[] axis);
    public native void wine_send_gamepad_button(int device, int button, int value);

    public native void wine_ime_start();
    public native void wine_ime_settext( String text, int length, int cursor );
    public native void wine_ime_finishtext();
    public native void wine_ime_canceltext();

    protected class WineInputConnection extends BaseInputConnection
    {
        KeyCharacterMap mKeyCharacterMap;

        public WineInputConnection( WineView targetView )
        {
            super( targetView, true);
        }

        public boolean beginBatchEdit()
        {
            Log.i("wine", "beginBatchEdit");
            return true;
        }

        public boolean clearMetaKeyStates (int states)
        {
            Log.i("wine", "clearMetaKeyStates");
            wine_clear_meta_key_states( states );
            return super.clearMetaKeyStates (states);
        }

        public boolean commitCompletion (CompletionInfo text)
        {
            Log.i("wine", "commitCompletion");
            return super.commitCompletion (text);
        }

        public boolean commitCorrection (CorrectionInfo correctionInfo)
        {
            Log.i("wine", "commitCorrection");
            return super.commitCorrection (correctionInfo);
        }

        public boolean commitText (CharSequence text, int newCursorPosition)
        {
            Log.i("wine", "commitText: '"+text.toString()+"'");

            super.commitText (text, newCursorPosition);

            /* This code based on BaseInputConnection in order to generate
                keystroke events for single character mappable input */
            Editable content = getEditable();
            if (content != null)
            {
                final int N = content.length();
                if (N == 1)
                {
                    // If it's 1 character, we have a chance of being
                    // able to generate normal key events...
                    if (mKeyCharacterMap == null)
                    {
                        mKeyCharacterMap = KeyCharacterMap.load(
                                KeyCharacterMap.BUILT_IN_KEYBOARD);
                    }
                    char[] chars = new char[1];
                    content.getChars(0, 1, chars, 0);
                    KeyEvent[] events = mKeyCharacterMap.getEvents(chars);
                    if (events != null)
                    {
                        wine_ime_canceltext();
                        for (int i=0; i<events.length; i++)
                        {
                            sendKeyEvent(events[i]);
                        }
                        content.clear();
                        return true;
                    }
                }

                wine_ime_start();
                wine_ime_settext( content.toString(), content.length(), newCursorPosition );
                wine_ime_finishtext();
                content.clear();
            }
            return true;
        }

        public boolean deleteSurroundingText (int beforeLength, int afterLength)
        {
            Log.i("wine", "deleteSurroundingText :"+beforeLength+","+afterLength);
            super.deleteSurroundingText (beforeLength, afterLength);
            return true;
        }

        public boolean endBatchEdit ()
        {
            Log.i("wine", "endBatchEdit");
            return true;
        }

        public boolean finishComposingText ()
        {
            Log.i("wine", "finishComposingText");
            wine_ime_finishtext();
            Editable content = getEditable();
            if (content != null)
                content.clear();
            return super.finishComposingText ();
        }

        public int getCursorCapsMode (int reqModes)
        {
            Log.i("wine", "getCursorCapsMode");
            return super.getCursorCapsMode (reqModes);
        }

        public Editable getEditable ()
        {
            Log.i("wine", "getEditable");
            return super.getEditable ();
        }

        public ExtractedText getExtractedText (ExtractedTextRequest request, int flags)
        {
            Log.i("wine", "getExtractedText");
            super.getExtractedText (request, flags);
            return null;
        }

        public CharSequence getSelectedText (int flags)
        {
            Log.i("wine", "getSelectedText");
            super.getSelectedText (flags);
            return null;
        }

        public CharSequence getTextAfterCursor (int length, int flags)
        {
            Log.i("wine", "getTextAfterCursor");
            return super.getTextAfterCursor (length, flags);
        }

        public CharSequence getTextBeforeCursor (int length, int flags)
        {
            Log.i("wine", "getTextBeforeCursor");
            return super.getTextBeforeCursor (length, flags);
        }

        public boolean performContextMenuAction (int id)
        {
            Log.i("wine", "performContextMenuAction");
            return super.performContextMenuAction (id);
        }

        public boolean performEditorAction (int actionCode)
        {
            Log.i("wine", "performEditorAction");
            return super.performEditorAction (actionCode);
        }

        public boolean performPrivateCommand (String action, Bundle data)
        {
            Log.i("wine", "performPrivateCommand");
            return super.performPrivateCommand (action, data);
        }

        public boolean reportFullscreenMode (boolean enabled)
        {
            Log.i("wine", "reportFullscreenMode");
            return super.reportFullscreenMode (enabled);
        }

        public boolean sendKeyEvent (KeyEvent event)
        {
            Log.i("wine", "sendKeyEvent");
            return super.sendKeyEvent (event);
        }

        public boolean setComposingRegion (int start, int end)
        {
            Log.i("wine", "setComposingRegion");
            return super.setComposingRegion (start, end);
        }

        public boolean setComposingText (CharSequence text, int newCursorPosition)
        {
            Log.i("wine", "setComposingText");
            Log.i("wine", "composeText: "+text.toString());
            wine_ime_start();
            wine_ime_settext( text.toString(), text.length(), newCursorPosition );
            return super.setComposingText (text, newCursorPosition);
        }

        public boolean setSelection (int start, int end)
        {
            Log.i("wine", "setSelection");
            return super.setSelection (start, end);
        }
    }

    public native boolean wine_keyboard_event( int hwnd, int action, int keycode, int scancode, int state );
    public native boolean wine_motion_event( int hwnd, int action, int x, int y, int state, int vscroll );
    public native void wine_surface_changed( int hwnd, Surface surface );
    public native void wine_desktop_changed( int width, int height );
    public native void wine_config_changed( int dpi, boolean force );
    public native void wine_clipboard_changed( boolean[] formats_present );
    public native void wine_import_clipboard_data( int index, byte[] data );
    public native void wine_clipboard_request( int index );
    private native void wine_run_commandline( String cmdline, String[] wineEnv );
    public native void wine_clear_meta_key_states( int states );

    //
    // Generic Wine window class
    //

    private HashMap<Integer,WineWindow> win_map = new HashMap<Integer,WineWindow>();
    private ArrayList<WineWindow> win_list = new ArrayList<WineWindow>();

    protected class WineWindow extends Object
    {
        static protected final int CLR_INVALID = 0xffffffff;
        static protected final int HWND_TOP = 0;
        static protected final int HWND_TOPMOST = 0xffffffff;
        static protected final int SWP_NOZORDER = 0x04;
        static protected final int SWP_SHOWWINDOW = 0x40;
        static protected final int SWP_HIDEWINDOW = 0x80;

        protected int hwnd;
        protected int owner;
        protected int region;
        protected int color_key;
        protected int alpha;
        protected boolean visible;
        protected boolean has_alpha;
        protected boolean use_gl;
        protected Rect rect;
        protected String text;
        protected BitmapDrawable icon;
        protected Surface surface;

        public WineWindow( int w )
        {
            Log.i( "wine", String.format( "create hwnd %08x", w ));
            hwnd = w;
            owner = 0;
            region = 0;
            color_key = CLR_INVALID;
            alpha = 0;
            visible = false;
            has_alpha = false;
            use_gl = false;
            rect = new Rect( 0, 0, 0, 0 );
            win_map.put( w, this );
        }

        public void destroy()
        {
            Log.i( "wine", String.format( "destroy hwnd %08x", hwnd ));
            if (visible)
            {
                visible = false;
                win_list.remove( this );
            }
            win_map.remove( this );
        }

        public void pos_changed( int flags, int insert_after, int new_owner, int style,
                                 int left, int top, int right, int bottom )
        {
            rect = new Rect( left, top, right, bottom );
            owner = new_owner;
            Log.i( "wine", String.format( "pos changed hwnd %08x after %08x owner %08x style %08x rect %s flags %08x",
                                          hwnd, insert_after, owner, style, rect, flags ));
            if (!visible && ((flags & SWP_SHOWWINDOW) != 0))
            {
                visible = true;
                win_list.add( this );
            }
            else if (visible && ((flags & SWP_HIDEWINDOW) != 0))
            {
                visible = false;
                win_list.remove( this );
            }
        }

        public void focus()
        {
            Log.i( "wine", String.format( "focus hwnd %08x", hwnd ));
        }

        public void set_text( String str )
        {
            Log.i( "wine", String.format( "set text hwnd %08x '%s'", hwnd, str ));
            text = str;
        }

        public void set_icon( BitmapDrawable bmp )
        {
            Log.i( "wine", String.format( "set icon hwnd %08x", hwnd ));
            icon = bmp;
        }

        public void set_region( int rgn )
        {
            Log.i( "wine", String.format( "set region hwnd %08x rgn %08x", hwnd, rgn ));
            region = rgn;
        }

        public void set_layered( int key, int a )
        {
            Log.i( "wine", String.format( "set layered hwnd %08x key %08x alpha %d", hwnd, key, a ));
            color_key = key;
            alpha = a;
        }

        public void set_alpha( boolean a )
        {
            Log.i( "wine", String.format( "set alpha hwnd %08x %b", hwnd, a ));
            has_alpha = a;
        }

        public void set_surface( SurfaceTexture surftex )
        {
            if (surftex == null) surface = null;
            else if (surface == null) surface = new Surface( surftex );
            Log.i( "wine", String.format( "set surface hwnd %08x %s", hwnd, surface ));
            wine_surface_changed( hwnd, surface );
        }

        public void start_opengl()
        {
            Log.i( "wine", String.format( "start opengl hwnd %08x", hwnd ));
            use_gl = true;
        }

        public boolean is_opaque()
        {
            return !has_alpha && region == 0 && color_key == CLR_INVALID;
        }

        public void get_event_pos( MotionEvent event, int[] pos )
        {
            pos[0] = Math.round( event.getRawX() );
            pos[1] = Math.round( event.getRawY() );
        }
    }

    //
    // Wine window implementation using a simple view for each window
    //

    protected class WineWindowView extends WineWindow
    {
        protected WineView view;

        public WineWindowView( Activity activity, int w )
        {
            super( w );
            view = new WineView( activity, this );
            if (top_view != null) top_view.addView( view );
        }

        public WineView get_view()
        {
            return view;
        }

        public void destroy()
        {
            top_view.removeView( view );
            view = null;
            super.destroy();
        }

        public void pos_changed( int flags, int insert_after, int new_owner, int style,
                                 int left, int top, int right, int bottom )
        {
            boolean show = !visible && ((flags & SWP_SHOWWINDOW) != 0);
            boolean hide = visible && ((flags & SWP_HIDEWINDOW) != 0);

            if (!show && (!visible || hide))
            {
                /* move it off-screen, except that if the texture still needs to be created
                 * we put one pixel on-screen */
                if (surface == null)
                    view.layout( left - right + 1, top - bottom + 1, 1, 1 );
                else
                    view.layout( left - right, top - bottom, 0, 0 );
            }
            else view.layout( left, top, right, bottom );

            if (show)
            {
                view.setFocusable( true );
                view.setFocusableInTouchMode( true );
            }
            else if (hide)
            {
                view.setFocusable( false );
                view.setFocusableInTouchMode( false );
            }

            if ((flags & SWP_NOZORDER) == 0 &&
                (insert_after == HWND_TOP || insert_after == HWND_TOPMOST))
                view.bringToFront();

            super.pos_changed( flags, insert_after, new_owner, style, left, top, right, bottom );
            top_view.update_action_bar();
        }

        public void focus()
        {
            super.focus();
            view.setFocusable( true );
            view.setFocusableInTouchMode( true );
            view.bringToFront();
            view.updateGamepads();
            top_view.update_action_bar();
        }

        public void set_text( String str )
        {
            super.set_text( str );
            top_view.update_action_bar();
        }

        public void set_icon( BitmapDrawable bmp )
        {
            super.set_icon( bmp );
            top_view.update_action_bar();
        }

        public void set_region( int rgn )
        {
            super.set_region( rgn );
            view.setOpaque( is_opaque() );
        }

        public void set_layered( int key, int a )
        {
            super.set_layered( key, a );
            view.setAlpha( a / 255.0f );
            view.setOpaque( is_opaque() );
        }

        public void set_alpha( boolean a )
        {
            super.set_alpha( a );
            view.setOpaque( is_opaque() );
        }

        public void set_surface( SurfaceTexture surftex )
        {
            // move it off-screen if we got a surface while not visible
            if (!visible && surface == null && surftex != null)
            {
                Log.i("wine",String.format("hwnd %08x not visible, moving offscreen", hwnd));
                view.layout( rect.left - rect.right, rect.top - rect.bottom, 0, 0 );
            }
            super.set_surface( surftex );
        }

        public void get_event_pos( MotionEvent event, int[] pos )
        {
            pos[0] = Math.round( event.getX() + view.getLeft() );
            pos[1] = Math.round( event.getY() + view.getTop() );
        }
    }

    protected class WineView extends TextureView implements TextureView.SurfaceTextureListener
    {
        static final int CLR_INVALID = 0xffffffff;

        private WineWindow window;

        public WineView( Activity act, WineWindow win )
        {
            super( act );
            window = win;
            setSurfaceTextureListener( this );
            setVisibility( View.VISIBLE );
        }

        public WineView( AttributeSet attrs )
        {
            super( getActivity(), attrs );
        }

        private final void updateGamepads()
        {
            ArrayList<Integer> gameControllerDeviceIds = new ArrayList<Integer>();
            ArrayList<String> gameControllerDeviceNames = new ArrayList<String>();
            int[] deviceIds = InputDevice.getDeviceIds();

            for (int deviceId : deviceIds)
            {
                InputDevice dev = InputDevice.getDevice(deviceId);
                int sources = dev.getSources();

                if (((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                || ((sources & InputDevice.SOURCE_JOYSTICK)
                == InputDevice.SOURCE_JOYSTICK))
                {
                    if (!gameControllerDeviceIds.contains(deviceId))
                    {
                        gameControllerDeviceIds.add(deviceId);
                        gameControllerDeviceNames.add(dev.getDescriptor());
                    }
                }
            }

            if (gameControllerDeviceIds.size() > 0)
            {
                int count = gameControllerDeviceIds.size();
                int i;

                wine_send_gamepad_count( count );
                for (i = 0; i < count; i++)
                {
                    int id = gameControllerDeviceIds.get(i);
                    String name = gameControllerDeviceNames.get(i);
                    wine_send_gamepad_data(i, id, name);
                }
            }
        }

        private float getCenteredAxis(MotionEvent event, InputDevice device, int axis)
        {
            final InputDevice.MotionRange range =
                device.getMotionRange(axis, event.getSource());

            if (range != null)
            {
                final float flat = range.getFlat();
                final float value = event.getAxisValue(axis);

                if (Math.abs(value) > flat)
                {
                    return value;
                }
            }
            return 0;
        }

        public boolean onGenericMotionEvent(MotionEvent event)
        {
            Log.i("wine", "view generic motion event");
            if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) ==
                InputDevice.SOURCE_JOYSTICK &&
                event.getAction() == MotionEvent.ACTION_MOVE)
                {
                    Log.i("wine", "Joystick Motion");
                    InputDevice mDevice = event.getDevice();
                    float[] axis;

                    axis = new float[10];
                    axis[0] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_X);
                    axis[1] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_Y);
                    axis[2] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_Z);
                    axis[3] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_RX);
                    axis[4] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_RY);
                    axis[5] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_RZ);
                    axis[6] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_LTRIGGER);
                    if (axis[6] == 0)
                        axis[6] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_BRAKE);
                    axis[7] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_RTRIGGER);
                    if (axis[7] == 0)
                        axis[7] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_GAS);
                    axis[8] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_HAT_X);
                    axis[9] = getCenteredAxis(event, mDevice,  MotionEvent.AXIS_HAT_Y);

                    wine_send_gamepad_axis(event.getDeviceId(), axis);
                    return true;
                }
            if ((event.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0)
            {
                int[] pos = new int[2];
                window.get_event_pos( event, pos );
                Log.i("wine", String.format( "view motion event win %08x action %d pos %d,%d buttons %04x view %d,%d",
                                             window.hwnd, event.getAction(), pos[0], pos[1], event.getButtonState(), getLeft(), getTop() ));
                return wine_motion_event( window.hwnd, event.getAction(), pos[0], pos[1], event.getButtonState(),
                                          (int)event.getAxisValue(MotionEvent.AXIS_VSCROLL)  );
            }
            return super.onGenericMotionEvent(event);
        }

        public boolean onKeyDown(int keyCode, KeyEvent event)
        {
            Log.i("wine", "Keydown");
            if ((event.getSource() & InputDevice.SOURCE_GAMEPAD)
                == InputDevice.SOURCE_GAMEPAD)
            {
                    Log.i("wine", "Is Gamepad "+keyCode);
                    wine_send_gamepad_button(event.getDeviceId(), keyCode, 0xff);
                    return true;
            }
            return super.onKeyDown(keyCode, event);
        }

        public boolean onKeyUp(int keyCode, KeyEvent event)
        {
            Log.i("wine", "KeyUp");
            if ((event.getSource() & InputDevice.SOURCE_GAMEPAD)
                == InputDevice.SOURCE_GAMEPAD)
            {
                    Log.i("wine", "Is Gamepad: "+keyCode);
                    wine_send_gamepad_button(event.getDeviceId(), keyCode, 0x0);
                    return true;
            }
            return super.onKeyDown(keyCode, event);
        }

        public boolean onTouchEvent( MotionEvent event )
        {
            int[] pos = new int[2];
            window.get_event_pos( event, pos );
            Log.i("wine", String.format( "view touch event win %08x action %d pos %d,%d buttons %04x view %d,%d",
                                         window.hwnd, event.getAction(), pos[0], pos[1], event.getButtonState(), getLeft(), getTop() ));
            return wine_motion_event( window.hwnd, event.getAction(), pos[0], pos[1], event.getButtonState(), 0 );
        }

        public boolean dispatchKeyEvent(KeyEvent event)
        {
            Log.i("wine", String.format( "view dispatchKeyEvent win %08x action %d keycode %d (%s)",
                                         window.hwnd, event.getAction(), event.getKeyCode(), event.keyCodeToString( event.getKeyCode() )));;
            boolean ret = wine_keyboard_event( window.hwnd, event.getAction(), event.getKeyCode(),
                                               event.getScanCode(), event.getMetaState() );
            if (!ret) ret = super.dispatchKeyEvent(event);
            return ret;
        }

        public void onSurfaceTextureAvailable(SurfaceTexture surftex, int width, int height)
        {
            Log.i("wine", String.format( "onSurfaceTextureAvailable win %08x %dx%d", window.hwnd, width, height ));
            window.set_surface( surftex );
        }

        public void onSurfaceTextureSizeChanged(SurfaceTexture surftex, int width, int height)
        {
            Log.i("wine", String.format( "onSurfaceTextureSizeChanged win %08x %dx%d", window.hwnd, width, height ));
            window.set_surface( surftex );
        }

        public boolean onSurfaceTextureDestroyed(SurfaceTexture surftex)
        {
            Log.i("wine", String.format( "onSurfaceTextureDestroyed win %08x", window.hwnd ));
            window.set_surface( null );
            return true;
        }

        public void onSurfaceTextureUpdated(SurfaceTexture surftex)
        {
        }

        public boolean onCheckIsTextEditor()
        {
            Log.i("wine", "onCheckIsTextEditor");
            return true;
        }

        public InputConnection onCreateInputConnection( EditorInfo outAttrs )
        {
            Log.i("wine", "onCreateInputConnection");
            outAttrs.inputType = InputType.TYPE_NULL;
            outAttrs.imeOptions = EditorInfo.IME_NULL;
            /* Disable voice for now. It double inputs until we can
               Support deletion of text in the document */
            outAttrs.privateImeOptions = "nm";
            return new WineInputConnection( this );
        }
    }

    protected class TopView extends ViewGroup implements ClipboardManager.OnPrimaryClipChangedListener
    {
        protected WineWindow desktop_win;
        protected WineView desktop_view;
        private ClipboardManager clipboard_manager;

        public TopView( int hwnd )
        {
            super( getActivity() );
            desktop_win = new WineWindow( hwnd );
            desktop_view = new WineView( getActivity(), desktop_win );
            desktop_win.visible = true;
            addView( desktop_view );

            clipboard_manager = (ClipboardManager)getActivity().getSystemService( Activity.CLIPBOARD_SERVICE );
            clipboard_manager.addPrimaryClipChangedListener( this );
            onPrimaryClipChanged();
        }

        @Override
        protected void onSizeChanged( int width, int height, int old_width, int old_height )
        {
            Log.i("wine", "desktop size " + width + "x" + height );
            desktop_view.layout( 0, 0, width, height );
            wine_desktop_changed( width, height );
        }

        @Override
        protected void onLayout( boolean changed, int left, int top, int right, int bottom )
        {
            /* nothing to do */
        }

        @Override
        public boolean dispatchKeyEvent( KeyEvent event )
        {
            return desktop_view.dispatchKeyEvent( event );
        }

        private void update_action_bar()
        {
            for (int i = getChildCount() - 1; i >= 0; i--)
            {
                View v = getChildAt( i );
                if (v instanceof WineView)
                {
                    WineView view = (WineView)v;
                    Log.i( "wine", String.format( "%d: %08x %s", i, view.window.hwnd, view.window.text ));
                    if (view.window.owner != 0) continue;
                    if (!view.window.visible) continue;
                    if (view == desktop_view) continue;
                    if (view.window.text == null) continue;
                    getActivity().setTitle( view.window.text );
                    if (getActivity().getActionBar() != null)
                    {
                        if (view.window.icon != null)
                            getActivity().getActionBar().setIcon( view.window.icon );
                        else
                            getActivity().getActionBar().setIcon( R.drawable.wine_launcher );
                    }
                    return;
                }
            }
            getActivity().setTitle( R.string.org_winehq_wine_app_name);
            if (getActivity().getActionBar() != null)
                getActivity().getActionBar().setIcon( R.drawable.wine_launcher );
        }

        private String[] format_mimetypes = { "text/plain" };

        // HashMap of mimetype to either:
        //  * An Integer identifying a mimetype, if Wine has copied data that we don't have yet.
        //  * A LinkedList of ParcelFileDescriptor objects to which we need to write the data.
        //  * A byte[] containing the data for this format.
        private HashMap<String, Object> copying = new HashMap<String, Object>();

        private String[] copying_mimetypes = null;

        private Uri get_clipboard_uri()
        {
            String authority = providerAuthority;
            if (authority == null)
            {
                return null;
            }
            else
            {
                return Uri.parse("content://" + authority + "/copying");
            }
        }

        public void onPrimaryClipChanged()
        {
            boolean[] formats_present = new boolean[format_mimetypes.length];

            ClipData clipdata = clipboard_manager.getPrimaryClip();
            ClipDescription clipdesc = null;

            if (clipdata != null)
            {
                clipdesc = clipdata.getDescription();

                if (clipdata.getItemCount() >= 1)
                {
                    ClipData.Item item = clipdata.getItemAt( 0 );

                    Uri uri = item.getUri();
                    if (uri != null && uri.equals(get_clipboard_uri()))
                        /* Wine holds the clipboard, ignore change. */
                        return;
                }
            }

            if (clipdesc != null)
            {
                for (int i=0; i < formats_present.length; i++)
                {
                    if (format_mimetypes[i] == "text/plain")
                        /* Android textboxes will try to paste anything, so let's match that. */
                        formats_present[i] = true;
                    else
                        formats_present[i] = clipdesc.hasMimeType( format_mimetypes[i] );
                }
            }

            wine_clipboard_changed( formats_present );
        }

        public void render_clipboard_data( int index )
        {
            String mimetype = format_mimetypes[index];
            byte[] data;

            Log.i( "wine", "render_clipboard_data " + index + " " + mimetype );

            ClipData clipdata = clipboard_manager.getPrimaryClip();

            if (clipdata == null)
            {
                data = new byte[0];
            }
            else if (mimetype == "text/plain")
            {
                Object[] str_list = new Object[clipdata.getItemCount()];

                for (int i=0; i < str_list.length; i++)
                {
                    str_list[i] = clipdata.getItemAt( i ).coerceToText( getActivity() );
                }

                String str_data = TextUtils.join( "\r\n", str_list );
                data = Charset.forName( "UTF-16LE" ).encode( str_data ).array();
            }
            else
            {
                data = new byte[0];
                /* FIXME: Use ContentResolver.openTypedAssetFileDescriptor ? */
            }

            wine_import_clipboard_data( index, data );
        }

        void really_acquire_clipboard( String[] mime_types ) throws IOException
        {
            // read any text before calling setPrimaryClip
            PipedInputStream in;
            PipedOutputStream out;

            in = new PipedInputStream();
            out = new PipedOutputStream(in);

            serveClipboardData( "text/plain", out );

            ByteArrayOutputStream bs = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int bytesread;

            do
            {
                bytesread = in.read(buffer);
                if (bytesread > 0)
                {
                    bs.write( buffer, 0, bytesread );
                }
            } while (bytesread > 0);

            CharSequence string = Charset.forName( "UTF-8" ).decode( ByteBuffer.wrap( bs.toByteArray() ) );

            Uri clipboard_uri = get_clipboard_uri();

            if (clipboard_uri == null)
            {
                Log.e( "wine", "can't export clipboard because WineDriver.setProviderAuthority wasn't called" );
                return;
            }

            ClipData.Item item = new ClipData.Item( string, null, clipboard_uri );

            final ClipData clipdata = new ClipData( "Wine", mime_types, item );

            runOnUiThread( new Runnable() { public void run() {
                clipboard_manager.setPrimaryClip( clipdata );
            }} );
        }

        void acquire_clipboard( boolean[] formats )
        {
            Log.i( "wine", "acquire_clipboard" );

            for (Object v : copying.values())
            {
                if (v instanceof LinkedList)
                {
                    LinkedList<OutputStream> ll = (LinkedList<OutputStream>)v;
                    for (OutputStream s : ll)
                    {
                        try { s.close(); } catch (IOException e) { }
                    }
                }
            }
            copying.clear();

            HashSet<String> mime_types = new HashSet<String>( formats.length );

            for (int i=0; i<formats.length; i++)
            {
                if (formats[i])
                {
                    Log.i( "wine", "adding mimetype "+format_mimetypes[i] );
                    mime_types.add( format_mimetypes[i] );
                    copying.put( format_mimetypes[i], (Integer)i );
                }
            }

            final String[] mime_type_array = mime_types.toArray( new String[0] );

            // Delaying the setPrimaryClip call in an ugly way so we don't deadlock.
            new Thread( new Runnable() { public void run() {
                try { really_acquire_clipboard( mime_type_array ); } catch (IOException e) { }
            }} ).start();
        }

        void send_clipboard_data( final OutputStream stream, final byte[] data )
        {
            new Thread( new Runnable() { public void run() {
                try { stream.write( data, 0, data.length ); } catch (IOException e) { }
                try { stream.close(); } catch (IOException e) { }
            }} ).start();
        }

        void export_clipboard_data( int index, byte[] data )
        {
            Log.i( "wine", "export_clipboard_data "+index );
            Object v = copying.put( format_mimetypes[index], data );
            if (v instanceof LinkedList)
            {
                LinkedList<OutputStream> ll = (LinkedList<OutputStream>)v;
                for (OutputStream s : ll)
                {
                    send_clipboard_data( s, data );
                }
            }
        }

        void serve_clipboard_data( String mimeType, OutputStream stream )
        {
            Object v = copying.get( mimeType );
            if (v instanceof byte[])
            {
                send_clipboard_data( stream, (byte[])v );
            }
            else if (v instanceof Integer)
            {
                LinkedList<OutputStream> ll = new LinkedList<OutputStream>();
                ll.add( stream );
                copying.put( mimeType, ll );
                wine_clipboard_request( ((Integer)v).intValue() );
            }
            else if (v instanceof LinkedList)
            {
                LinkedList<OutputStream> ll = (LinkedList<OutputStream>)v;
                ll.add( stream );
            }
            else
            {
                /* probably null */
                try { stream.close(); } catch (IOException e) { }
            }
        }
    }

    protected static TopView top_view;

    protected WineWindow get_window( int hwnd )
    {
        return win_map.get( hwnd );
    }

    public void create_desktop_window( int hwnd )
    {
        if (progDlg != null) progDlg.dismiss();
        Log.i( "wine", "create desktop view " + String.format("%08x",hwnd));
        top_view = new TopView( hwnd );
        if (startupDpi == 0)
            startupDpi = activity.getResources().getConfiguration().densityDpi;
        wine_config_changed( startupDpi, firstRun );
    }

    public void create_window( int hwnd, String wingroup )
    {
        if (get_window( hwnd ) != null) return;  /* already exists */
        WineWindowView win = new WineWindowView( activity, hwnd );
    }

    public void destroy_window( int hwnd )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.destroy();
    }

    public void focus_window( int hwnd )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.focus();
    }

    public void set_window_icon( int hwnd, int width, int height, int icon[] )
    {
        WineWindow win = get_window( hwnd );
        if (win == null) return;
        BitmapDrawable new_icon = null;
        if (icon != null) new_icon = new BitmapDrawable( activity.getResources(),
                                                         Bitmap.createBitmap( icon, width, height, Bitmap.Config.ARGB_8888 ) );
        win.set_icon( new_icon );
    }

    public void set_window_text( int hwnd, String text )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.set_text( text );
    }

    public void window_pos_changed( int hwnd, int flags, int insert_after, int owner, int style,
                                    int left, int top, int right, int bottom )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.pos_changed( flags, insert_after, owner, style, left, top, right, bottom );
    }

    public void set_window_region( int hwnd, int rgn )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.set_region( rgn );
    }

    public void set_window_layered( int hwnd, int key, int alpha )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.set_layered( key, alpha );
    }

    public void set_window_surface( int hwnd, boolean has_alpha )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.set_alpha( has_alpha );
    }

    public void start_window_opengl( int hwnd )
    {
        WineWindow win = get_window( hwnd );
        if (win != null) win.start_opengl();
    }

    public void render_clipboard_data( int index )
    {
        top_view.render_clipboard_data( index );
    }

    public void acquire_clipboard( boolean[] formats )
    {
        top_view.acquire_clipboard( formats );
    }

    public void export_clipboard_data( int index, byte[] data )
    {
        top_view.export_clipboard_data( index, data );
    }


    public void createDesktopWindow( final int hwnd )
    {
        runOnUiThread( new Runnable() { public void run() { create_desktop_window( hwnd ); }} );
    }

    public void createWindow( final int hwnd, final String wingroup )
    {
        runOnUiThread( new Runnable() { public void run() { create_window( hwnd, wingroup ); }} );
    }

    public void destroyWindow( final int hwnd )
    {
        runOnUiThread( new Runnable() { public void run() { destroy_window( hwnd ); }} );
    }

    public void startOpenGL( final int hwnd )
    {
        runOnUiThread( new Runnable() { public void run() { start_window_opengl( hwnd ); }} );
    }

    public void setFocus( final int hwnd )
    {
        runOnUiThread( new Runnable() { public void run() { focus_window( hwnd ); }} );
    }

    public void setWindowIcon( final int hwnd, final int width, final int height, final int icon[] )
    {
        runOnUiThread( new Runnable() { public void run() { set_window_icon( hwnd, width, height, icon ); }} );
    }

    public void setWindowText( final int hwnd, final String text )
    {
        runOnUiThread( new Runnable() { public void run() { set_window_text( hwnd, text ); }} );
    }

    public void windowPosChanged( final int hwnd, final int flags, final int insert_after,
                                  final int owner, final int style,
                                  final int left, final int top, final int right, final int bottom )
    {
        runOnUiThread( new Runnable() {
            public void run() { window_pos_changed( hwnd, flags, insert_after, owner, style,
                                                    left, top, right, bottom ); }} );
    }

    public void setWindowRgn( final int hwnd, final int region )
    {
        runOnUiThread( new Runnable() { public void run() { set_window_region( hwnd, region ); }} );
    }

    public void setWindowLayered( final int hwnd, final int key, final int alpha )
    {
        runOnUiThread( new Runnable() { public void run() { set_window_layered( hwnd, key, alpha ); }} );
    }

    public void setWindowSurface( final int hwnd, final boolean has_alpha )
    {
        runOnUiThread( new Runnable() { public void run() { set_window_surface( hwnd, has_alpha ); }} );
    }

    public void renderClipboardData( final int index )
    {
        new Thread( new Runnable() { public void run() { render_clipboard_data( index ); }} ).start();
    }

    public void acquireClipboard( final boolean[] formats )
    {
        runOnUiThread( new Runnable() { public void run() { acquire_clipboard( formats ); }} );
    }

    public void exportClipboardData( final int index, final byte[] data )
    {
        runOnUiThread( new Runnable() { public void run() { export_clipboard_data( index, data ); }} );
    }

    public static String[] getCopyingTypes( final String mimeTypeFilter )
    {
        String[] copying_mimetypes = null;

        if (clipboard_driver != null && clipboard_driver.top_view != null)
            copying_mimetypes = clipboard_driver.top_view.copying_mimetypes;
        if (copying_mimetypes == null)
            copying_mimetypes = new String[] { "text/plain" };
        else
            copying_mimetypes = copying_mimetypes.clone();

        int i=0, length=copying_mimetypes.length;
        while (i < length)
        {
            if (ClipDescription.compareMimeTypes( copying_mimetypes[i], mimeTypeFilter ))
            {
                i++;
            }
            else
            {
                copying_mimetypes[i] = copying_mimetypes[length-1];
                length--;
            }
        }

        if (length == 0)
            return null;
        else if (length == copying_mimetypes.length)
            return copying_mimetypes;
        else
            return Arrays.copyOf( copying_mimetypes, length );
    }

    public static void serveClipboardData( String mimeTypeFilter, final OutputStream stream )
    {
        String[] mimeTypes = getCopyingTypes( mimeTypeFilter );
        if (mimeTypes != null && clipboard_driver != null)
        {
            final String mimeType = mimeTypes[0];
            final TopView top_view = clipboard_driver.top_view;
            if (top_view != null)
            {
                clipboard_driver.runOnUiThread( new Runnable() { public void run() { top_view.serve_clipboard_data( mimeType, stream ); }} );
            }
        }
        else
        {
            try { stream.close(); } catch (IOException e) { }
        }
    }
}
