using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace FofPrivate {
// Fixed v6 minimum size, reduced only if required by the monitor's work area.
static class WindowFrame {
    [StructLayout(LayoutKind.Sequential)] internal struct Point { public int X,Y; public Point(int x,int y){X=x;Y=y;} }
    [StructLayout(LayoutKind.Sequential)] internal struct Rect { public int Left,Top,Right,Bottom; public Rect(int l,int t,int r,int b){Left=l;Top=t;Right=r;Bottom=b;} }
    [StructLayout(LayoutKind.Sequential)] internal struct MinMax { public Point Reserved,MaxSize,MaxPosition,MinTrackSize,MaxTrackSize; }
    [StructLayout(LayoutKind.Sequential)] struct MonitorInfo { public int Size; public Rect Monitor,Work; public uint Flags; }
    [DllImport("user32.dll")] static extern IntPtr MonitorFromWindow(IntPtr hwnd,uint flags);
    [DllImport("user32.dll",CharSet=CharSet.Auto)] static extern bool GetMonitorInfo(IntPtr monitor,ref MonitorInfo info);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd,out Rect rect);
    [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] static extern IntPtr GetWindowLongPtr(IntPtr hwnd,int index);
    [DllImport("user32.dll",EntryPoint="SetWindowLongPtrW")] static extern IntPtr SetWindowLongPtr(IntPtr hwnd,int index,IntPtr value);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd,int message,IntPtr wparam,IntPtr lparam);
    static MonitorInfo Monitor(IntPtr hwnd){var info=new MonitorInfo{Size=Marshal.SizeOf(typeof(MonitorInfo))};if(!GetMonitorInfo(MonitorFromWindow(hwnd,2),ref info))throw new InvalidOperationException("Cannot read monitor bounds");return info;}
    internal static MinMax Constrain(MinMax value,Rect monitor,Rect work){value.MaxPosition=new Point(work.Left-monitor.Left,work.Top-monitor.Top);value.MaxSize=new Point(work.Right-work.Left,work.Bottom-work.Top);return value;}
    static void Limits(Window window,HwndSource source){
        var info=Monitor(source.Handle);var size=source.CompositionTarget.TransformFromDevice.Transform(new System.Windows.Point(info.Work.Right-info.Work.Left,info.Work.Bottom-info.Work.Top));
        double width=Math.Min(900,Math.Max(1,size.X-24)),height=Math.Min(600,Math.Max(1,size.Y-24));
        window.MinWidth=Math.Min(window.MinWidth,width);window.MinHeight=Math.Min(window.MinHeight,height);
        window.MaxWidth=width;window.MaxHeight=height;window.MinWidth=width;window.MinHeight=height;
        if(window.Width!=width)window.Width=width;if(window.Height!=height)window.Height=height;
    }
    internal static void Install(Window window){
        window.SourceInitialized+=delegate{
            var source=HwndSource.FromHwnd(new WindowInteropHelper(window).Handle);Limits(window,source);
            long style=GetWindowLongPtr(source.Handle,-16).ToInt64();SetWindowLongPtr(source.Handle,-16,new IntPtr(style&~0x00050000L));
            source.AddHook(delegate(IntPtr hwnd,int message,IntPtr wparam,IntPtr lparam,ref bool handled){
                if((message==0x112&&((wparam.ToInt64()&0xFFF0)==0xF030||(wparam.ToInt64()&0xFFF0)==0xF000))||(message==0xA3&&wparam.ToInt64()==2)){handled=true;return IntPtr.Zero;}
                if(message==0x24){var info=Monitor(hwnd);var value=Constrain((MinMax)Marshal.PtrToStructure(lparam,typeof(MinMax)),info.Monitor,info.Work);var dpi=source.CompositionTarget.TransformToDevice;value.MinTrackSize=new Point((int)Math.Ceiling(window.MinWidth*dpi.M11),(int)Math.Ceiling(window.MinHeight*dpi.M22));value.MaxTrackSize=new Point((int)Math.Floor(window.MaxWidth*dpi.M11),(int)Math.Floor(window.MaxHeight*dpi.M22));Marshal.StructureToPtr(value,lparam,false);handled=true;}
                if(message==0x02E0)source.Dispatcher.BeginInvoke(new Action(()=>Limits(window,source)));
                return IntPtr.Zero;
            });
            window.LocationChanged+=delegate{Limits(window,source);};
        };
        window.StateChanged+=delegate{if(window.WindowState==WindowState.Maximized)window.WindowState=WindowState.Normal;};
    }
    internal static void FitInitial(Window window){window.WindowStartupLocation=WindowStartupLocation.CenterScreen;}
    internal static void Check(Window window){
        IntPtr hwnd=new WindowInteropHelper(window).Handle;var source=HwndSource.FromHwnd(hwnd);var dpi=source.CompositionTarget.TransformToDevice;
        if((GetWindowLongPtr(hwnd,-16).ToInt64()&0x00050000L)!=0)throw new Exception("Native resize/maximize style remains enabled");
        if(window.MinWidth!=window.MaxWidth||window.MinHeight!=window.MaxHeight||window.ResizeMode!=ResizeMode.CanMinimize)throw new Exception("Window is not fixed size");
        double oldWidth=window.Width,oldHeight=window.Height;
        window.Width=1;window.Height=1;window.Dispatcher.Invoke(DispatcherPriority.ApplicationIdle,new Action(window.UpdateLayout));Rect small;GetWindowRect(hwnd,out small);
        if(Math.Abs((small.Right-small.Left)-window.MinWidth*dpi.M11)>2||Math.Abs((small.Bottom-small.Top)-window.MinHeight*dpi.M22)>2)throw new Exception("Minimum resize limits failed");
        window.Width=10000;window.Height=10000;window.Dispatcher.Invoke(DispatcherPriority.ApplicationIdle,new Action(window.UpdateLayout));Rect large;GetWindowRect(hwnd,out large);
        if((large.Right-large.Left)>window.MaxWidth*dpi.M11+2||(large.Bottom-large.Top)>window.MaxHeight*dpi.M22+2)throw new Exception("Maximum resize limits failed");
        SendMessage(hwnd,0x112,new IntPtr(0xF030),IntPtr.Zero);window.Dispatcher.Invoke(DispatcherPriority.ApplicationIdle,new Action(window.UpdateLayout));if(window.WindowState==WindowState.Maximized)throw new Exception("System maximize was not blocked");
        SendMessage(hwnd,0xA3,new IntPtr(2),IntPtr.Zero);window.Dispatcher.Invoke(DispatcherPriority.ApplicationIdle,new Action(window.UpdateLayout));if(window.WindowState==WindowState.Maximized)throw new Exception("Caption double-click was not blocked");
        window.WindowState=WindowState.Maximized;window.Dispatcher.Invoke(DispatcherPriority.ApplicationIdle,new Action(window.UpdateLayout));if(window.WindowState!=WindowState.Normal)throw new Exception("Programmatic maximize was not blocked");
        window.Width=oldWidth;window.Height=oldHeight;window.UpdateLayout();
        Console.WriteLine("WINDOW_FRAME_PASS fixed-size/no-resize-style/no-maximize-style/maximize-command-blocked");
    }
}
}
