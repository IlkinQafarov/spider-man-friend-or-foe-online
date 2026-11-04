using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Markup;
using System.Windows.Threading;
using Ellipse = System.Windows.Shapes.Ellipse;

namespace FofPrivate {
// The WPF shell owns presentation only. LauncherForm remains the established session backend.
public static class WpfLauncher {
    private const string UiResource = "fof.ui";

    public static int Run(bool review) {
        Application app = Application.Current;
        if (app == null) app = new Application();
        app.ShutdownMode = ShutdownMode.OnMainWindowClose;
        LauncherForm backend = new LauncherForm(review);
        Window window = LoadWindow();
        app.MainWindow = window;
        WindowFrame.FitInitial(window);
        ComicWindow shell = new ComicWindow(window, backend, review);
        shell.Initialize();
        if (!review) backend.StartBackgroundDiscovery();
        return app.Run(window);
    }

    // Offline visual-review hook: only XAML/art and the inert review backend are loaded.
    public static int Preview(string path, string language, bool small, string flags) {
        if (String.IsNullOrWhiteSpace(path)) throw new ArgumentException("Preview output path is required.");
        Application app = Application.Current;
        if (app == null) app = new Application();
        app.ShutdownMode = ShutdownMode.OnExplicitShutdown;
        LauncherForm backend = new LauncherForm(true);
        Window window = LoadWindow();
        flags=flags??"";
        window.Width = small ? 900 : flags.Contains("max")||flags.Contains("wide")?1440:flags.Contains("tall")?900:1200;
        window.Height = small||flags.Contains("wide") ? 600 : flags.Contains("max")||flags.Contains("tall")?960:800;
        window.Left = -12000;
        window.Top = -12000;
        window.ShowInTaskbar = false;
        app.MainWindow = window;
        ComicWindow shell = new ComicWindow(window, backend, true);
        shell.Initialize();
        if (!String.IsNullOrEmpty(language)) shell.SetLanguage(language);
        window.Show();
        shell.ConfigurePreview(flags);
        window.UpdateLayout();
        bool artboard = flags.Contains("native");
        // Capture the laid-out root, including uniform-fit gutters and border, without stretching Stage.
        FrameworkElement content = artboard ? (FrameworkElement)window.FindName("Stage") : (FrameworkElement)window.Content;
        content.UpdateLayout();
        double scale=flags.Contains("dpi200")?2:flags.Contains("dpi150")?1.5:flags.Contains("dpi125")?1.25:1;
        double logicalWidth=content.ActualWidth,logicalHeight=content.ActualHeight;
        int width=(int)Math.Ceiling(logicalWidth*scale),height=(int)Math.Ceiling(logicalHeight*scale);
        RenderTargetBitmap bitmap = new RenderTargetBitmap(width, height, 96*scale, 96*scale, PixelFormats.Pbgra32);
        bitmap.Render(content);
        PngBitmapEncoder encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (FileStream output = new FileStream(Path.GetFullPath(path), FileMode.Create, FileAccess.Write, FileShare.None)) encoder.Save(output);
        shell.PreparePreviewClose();
        window.Close();
        backend.Dispose();
        return 0;
    }

    public static int Selftest(){Application app=Application.Current??new Application();app.ShutdownMode=ShutdownMode.OnExplicitShutdown;using(var backend=new LauncherForm(true)){Window window=LoadWindow();window.ShowInTaskbar=false;window.ShowActivated=false;window.Opacity=0;window.Left=-12000;window.Top=-12000;ComicWindow shell=new ComicWindow(window,backend,true);shell.Initialize();window.Show();window.UpdateLayout();WindowFrame.Check(window);shell.CheckRoofMotion();shell.CheckUi();shell.PreparePreviewClose();window.Close();}Console.WriteLine("WPF_UI_SELFTEST_PASS rooftop-geometry/11-locales/accessible-labels-and-status/20-digit-session-fit/panels/join-validation/cancel/settings/keyboard/three-layers/no-account-no-game");return 0;}

    private static Window LoadWindow() {
        using (Stream stream = typeof(WpfLauncher).Assembly.GetManifestResourceStream(UiResource)) {
            if (stream == null) throw new InvalidOperationException("Embedded WPF window resource fof.ui is missing.");
            Window window = XamlReader.Load(stream) as Window;
            if (window == null) throw new InvalidDataException("Embedded WPF resource is not a Window.");
            return window;
        }
    }
}

internal sealed partial class ComicWindow {
    private readonly Window window;
    private readonly LauncherForm backend;
    private readonly bool review;
    private readonly DispatcherTimer timer;
    private bool settingUp, allowClose, closing;
    private IInputElement cardReturnFocus;
    private int seenFriendJoined;
    private Window friendNotice;
    [System.Runtime.InteropServices.DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] static extern IntPtr NoticeGetStyle(IntPtr hwnd,int index);
    [System.Runtime.InteropServices.DllImport("user32.dll",EntryPoint="SetWindowLongPtrW")] static extern IntPtr NoticeSetStyle(IntPtr hwnd,int index,IntPtr value);
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    private static Brush NoticePaper(){
        var bitmap=new BitmapImage();using(var stream=typeof(ComicWindow).Assembly.GetManifestResourceStream("fof.paper.lobby")){
            bitmap.BeginInit();bitmap.CacheOption=BitmapCacheOption.OnLoad;bitmap.StreamSource=stream;bitmap.EndInit();bitmap.Freeze();
        }
        var crop=new CroppedBitmap(bitmap,new Int32Rect(20,70,2130,560));crop.Freeze();
        return new ImageBrush(crop){Stretch=Stretch.Fill};
    }
    private static Window CreateFriendNotice(){
        var notice=new Window{Width=420,Height=112,WindowStyle=WindowStyle.None,ResizeMode=ResizeMode.NoResize,
            ShowInTaskbar=false,ShowActivated=false,Topmost=true,IsHitTestVisible=false,Focusable=false,
            Background=Brushes.Transparent,AllowsTransparency=true,Content=new Border{Background=NoticePaper(),BorderBrush=Brushes.Transparent,BorderThickness=new Thickness(0),Padding=new Thickness(22,35,22,12),
                Child=new TextBlock{Text=UiText.Get("friendJoined"),FontFamily=new FontFamily("Segoe UI"),FontSize=22,FontWeight=FontWeights.SemiBold,Foreground=Brushes.Black,TextWrapping=TextWrapping.Wrap,VerticalAlignment=VerticalAlignment.Center}}};
        notice.SourceInitialized+=delegate{var handle=new System.Windows.Interop.WindowInteropHelper(notice).Handle;
            NoticeSetStyle(handle,-20,new IntPtr(NoticeGetStyle(handle,-20).ToInt64()|0x08000020L));};
        return notice;
    }
    private void ShowFriendNotice(){
        if(friendNotice!=null)friendNotice.Close();
        var notice=CreateFriendNotice();friendNotice=notice;
        var area=SystemParameters.WorkArea;notice.Left=area.Right-notice.Width-20;notice.Top=area.Top+20;
        var dismiss=new DispatcherTimer{Interval=TimeSpan.FromSeconds(7)};
        dismiss.Tick+=delegate{dismiss.Stop();notice.Close();};
        notice.Closed+=delegate{dismiss.Stop();if(friendNotice==notice)friendNotice=null;};
        notice.Show();dismiss.Start();
    }

    internal ComicWindow(Window window, LauncherForm backend, bool review) {
        this.window = window; this.backend = backend; this.review = review;
        timer = new DispatcherTimer(DispatcherPriority.Background, window.Dispatcher);
        timer.Interval = TimeSpan.FromMilliseconds(200);
        timer.Tick += delegate { Refresh(); };
    }

    private T Get<T>(string name) where T : class {
        T control = window.FindName(name) as T;
        if (control == null) throw new InvalidDataException("Missing WPF control: " + name);
        return control;
    }

    internal void Initialize() {
        WindowFrame.Install(window);
        LoadArt();
        InstallPaperPanels();
        SetupChoices();
        if (!review) InitializeSounds();
        WireActions();
        ApplyTexts();
        Refresh();
        InitializeAccessibility();
        timer.Start();
        window.PreviewKeyDown += WindowKeyDown;
        window.Closing += WindowClosing;
        window.Closed += delegate { timer.Stop(); };
    }

    private readonly System.Collections.Generic.List<Image> paperPanels = new System.Collections.Generic.List<Image>();
    private void InstallPaperPanels() {
        foreach(string name in new[]{"SessionBar","JoinCard","SettingsCard"}) {
            var panel=Get<Border>(name);var grid=(Grid)panel.Child;
            bool lobby=name=="SessionBar";
            BitmapImage bitmap=new BitmapImage();
            using(var stream=typeof(ComicWindow).Assembly.GetManifestResourceStream(lobby?"fof.paper.lobby":"fof.paper.dialog")){
                bitmap.BeginInit();bitmap.CacheOption=BitmapCacheOption.OnLoad;bitmap.StreamSource=stream;bitmap.EndInit();bitmap.Freeze();
            }
            // Nine-slice rendering keeps web corners and ink edges at their intended size.
            var source=new CroppedBitmap(bitmap,lobby?new Int32Rect(20,70,2130,560):new Int32Rect(16,45,1505,940));source.Freeze();
            var image=new Image{IsHitTestVisible=false,Stretch=Stretch.Fill};Grid.SetRowSpan(image,99);Panel.SetZIndex(image,-1);grid.Children.Insert(0,image);paperPanels.Add(image);
            Action draw=delegate{
                double w=grid.ActualWidth,h=grid.ActualHeight;if(w<=0||h<=0)return;
                int sx=lobby?250:210,sy=lobby?180:145,bottom=55;
                double dx=lobby?64:100,dy=lobby?36:100,db=25;
                int[] xs={0,sx,source.PixelWidth-sx,source.PixelWidth},ys={0,sy,source.PixelHeight-bottom,source.PixelHeight};
                double[] xd={0,dx,w-dx,w},yd={0,dy,h-db,h};
                var drawing=new DrawingGroup();using(var dc=drawing.Open())for(int y=0;y<3;y++)for(int x=0;x<3;x++){
                    // The body has narrow ink rails; keep its broad paper texture in one piece.
                    int[] rowX=y==1?new[]{0,24,source.PixelWidth-24,source.PixelWidth}:xs;
                    double[] rowD=y==1?new[]{0.0,16.0,w-16,w}:xd;
                    var piece=new CroppedBitmap(source,new Int32Rect(rowX[x],ys[y],rowX[x+1]-rowX[x],ys[y+1]-ys[y]));piece.Freeze();
                    dc.DrawImage(piece,new Rect(rowD[x],yd[y],rowD[x+1]-rowD[x]+0.5,yd[y+1]-yd[y]+0.5));
                }
                drawing.Freeze();image.Source=new DrawingImage(drawing);
            };
            grid.SizeChanged+=delegate{draw();};draw();
        }
    }

    private readonly System.Collections.Generic.Dictionary<string, System.Media.SoundPlayer> sounds =
        new System.Collections.Generic.Dictionary<string, System.Media.SoundPlayer>();

    private ButtonBase pressCueTarget;

    private void InitializeSounds() {
        foreach (string cue in new[]{"arrival", "close", "click"}) {
            var stream = typeof(ComicWindow).Assembly.GetManifestResourceStream("fof.sound." + cue);
            if (stream == null) continue;
            var player = new System.Media.SoundPlayer(stream);
            try {
                player.Load();
                sounds.Add(cue, player);
            } catch (InvalidOperationException) {
                player.Dispose();
            } finally {
                stream.Dispose();
            }
        }
        window.ContentRendered += delegate { PlayCue("arrival"); };
        var settingsTabs = Get<TabControl>("SettingsTabs");
        settingsTabs.SelectionChanged += delegate(object sender, SelectionChangedEventArgs e) {
            // ComboBox selection events also bubble through the TabControl.
            if (e.OriginalSource == settingsTabs && settingsTabs.IsVisible) PlayCue("click");
        };
        // Give feedback on press; button commands still follow their normal release semantics.
        window.PreviewMouseDown += delegate(object sender, MouseButtonEventArgs e) {
            if (e.ChangedButton == MouseButton.Left) SoundButtonPress(e.OriginalSource as DependencyObject);
        };
        window.PreviewMouseUp += delegate(object sender, MouseButtonEventArgs e) {
            if (e.ChangedButton == MouseButton.Left) FinishSoundPress();
        };
        window.PreviewKeyDown += delegate(object sender, KeyEventArgs e) {
            if (!e.IsRepeat && (e.Key == Key.Space || e.Key == Key.Enter))
                SoundButtonPress(Keyboard.FocusedElement as ButtonBase);
        };
        window.PreviewKeyUp += delegate(object sender, KeyEventArgs e) {
            if (e.Key == Key.Space || e.Key == Key.Enter) FinishSoundPress();
        };
        window.AddHandler(ButtonBase.ClickEvent, new RoutedEventHandler(delegate(object sender, RoutedEventArgs e) {
            var button = e.OriginalSource as ButtonBase;
            if (button != null && button != pressCueTarget && button != Get<Button>("CloseButton"))
                PlayCue("click"); // Activation without a preceding press, such as UI Automation.
        }), true);
        window.Closed += delegate {
            // The process exits after Closed returns. Finish this short cue before disposing its player.
            System.Media.SoundPlayer exitSound;
            if (sounds.TryGetValue("close", out exitSound)) exitSound.PlaySync();
            foreach (var player in sounds.Values) player.Dispose();
            sounds.Clear();
        };
    }

    private void SoundButtonPress(DependencyObject source) {
        while (source != null && !(source is ButtonBase))
            source = source is Visual ? VisualTreeHelper.GetParent(source) : LogicalTreeHelper.GetParent(source);
        pressCueTarget = source as ButtonBase;
        if (pressCueTarget != null && pressCueTarget.IsEnabled && pressCueTarget != Get<Button>("CloseButton"))
            PlayCue("click");
    }

    private void FinishSoundPress() {
        // Click can still bubble during this release. Clear the guard after routing completes.
        window.Dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(delegate { pressCueTarget = null; }));
    }

    private void PlayCue(string cue) {
        System.Media.SoundPlayer player;
        if (sounds.TryGetValue(cue, out player)) player.Play();
    }

    internal void PreparePreviewClose(){allowClose=true;timer.Stop();}

    private readonly TranslateTransform cityTravel = new TranslateTransform();
    private readonly RotateTransform signSwing = new RotateTransform(0, 768, 0);
    private double targetX, targetY, swingTime;
    private TimeSpan? previousFrame;
    private bool roofAnimating;

    // Column zero retains the English lettering; column one is clean paper for live labels.
    private readonly BitmapSource[,] hoverSigns = new BitmapSource[2,2];

    private void LoadHoverSigns() {
        using (var stream = typeof(ComicWindow).Assembly.GetManifestResourceStream("fof.hover.signs")) {
            var atlas = BitmapFrame.Create(stream, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            for (int sign = 0; sign < 2; ++sign) {
                var image = Get<Image>(sign == 0 ? "HoverHost" : "HoverJoin");
                for (int variant = 0; variant < 2; ++variant) {
                    var bounds = new Int32Rect(variant * atlas.PixelWidth / 2, sign * atlas.PixelHeight / 2,
                        (int)image.Width, (int)image.Height);
                    var tile = new CroppedBitmap(atlas, bounds);
                    tile.Freeze();
                    hoverSigns[sign,variant] = tile;
                }
            }
        }
    }

    private void LoadArt() {
        string art = Path.Combine(Files.Root, "launcher", "assets", "rooftop");
        SetImage("RoofCity", Path.Combine(art, "distant-city.png"));
        SetImage("RoofTitle", Path.Combine(art, "swing-sign.png"));
        SetImage("RoofFront", Path.Combine(art, "front.png"));
        BitmapImage plates = ReadImage(Path.Combine(art, "live-plates.png"));
        foreach (string name in new[]{"RoofHostPlate","RoofJoinPlate","RoofLanguagePlate","RoofStatusPlate"}) {
            Image plate = Get<Image>(name);
            var bounds = new Int32Rect((int)Canvas.GetLeft(plate), (int)Canvas.GetTop(plate), (int)plate.Width, (int)plate.Height);
            var crop = new CroppedBitmap(plates, bounds);
            crop.Freeze();
            plate.Source = crop;
        }
        LoadHoverSigns();
        using (var stream = typeof(ComicWindow).Assembly.GetManifestResourceStream("fof.hover.ink")) {
            var ink = BitmapFrame.Create(stream, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            foreach (string name in new[]{"InkGlobe","InkEnglish","InkArrow","InkSettings","InkAccess"}) {
                var image = Get<Image>(name);
                var bounds = new Int32Rect((int)Canvas.GetLeft(image), (int)Canvas.GetTop(image) - 914,
                    (int)image.Width, (int)image.Height);
                var crop = new CroppedBitmap(ink, bounds);
                crop.Freeze();
                image.Source = crop;
            }
        }
        var transform = new TransformGroup();
        transform.Children.Add(new ScaleTransform(1.014, 1.014, 768, 512));
        transform.Children.Add(cityTravel);
        Get<Image>("RoofCity").RenderTransform = transform;
        Get<Image>("RoofTitle").RenderTransform = signSwing;
        Canvas roof = Get<Canvas>("RoofCanvas");
        roof.MouseMove += delegate(object sender, MouseEventArgs e) {
            if (!roofAnimating || e.StylusDevice != null) return;
            AimRoof(e.GetPosition(roof));
        };
        roof.MouseLeave += delegate { targetX = targetY = 0; };
        roof.IsEnabledChanged += delegate { UpdateRoofMotion(); };
        window.Activated += delegate { UpdateRoofMotion(); };
        window.Deactivated += delegate { StopRoofMotion(); };
        window.StateChanged += delegate { UpdateRoofMotion(); };
        Get<Grid>("Overlay").IsVisibleChanged += delegate { UpdateRoofMotion(); };
        Get<ComboBox>("LanguageBox").DropDownOpened += delegate { StopRoofMotion(); };
        Get<ComboBox>("LanguageBox").DropDownClosed += delegate { UpdateRoofMotion(); };
        SystemParameters.StaticPropertyChanged += RoofSystemChanged;
        window.Closed += delegate {
            SystemParameters.StaticPropertyChanged -= RoofSystemChanged;
            StopRoofMotion();
        };
    }

    private void RoofSystemChanged(object sender, System.ComponentModel.PropertyChangedEventArgs args) {
        if (args.PropertyName == "ClientAreaAnimation") UpdateRoofMotion();
    }

    private void UpdateRoofMotion() {
        bool enabled = window.IsActive && window.WindowState != WindowState.Minimized &&
            !accessible && SystemParameters.ClientAreaAnimation &&
            Get<Canvas>("RoofCanvas").IsEnabled && Get<Grid>("Overlay").Visibility != Visibility.Visible &&
            !Get<ComboBox>("LanguageBox").IsDropDownOpen;
        if (!enabled) { StopRoofMotion(); return; }
        if (!roofAnimating) {
            roofAnimating = true;
            CompositionTarget.Rendering += RenderRoof;
        }
    }

    private void StopRoofMotion() {
        if (roofAnimating) CompositionTarget.Rendering -= RenderRoof;
        roofAnimating = false;
        previousFrame = null;
        targetX = targetY = cityTravel.X = cityTravel.Y = signSwing.Angle = swingTime = 0;
    }

    private void AimRoof(Point point) {
        targetX = -9 * Math.Max(-1, Math.Min(1, point.X / 768 - 1));
        targetY = -5 * Math.Max(-1, Math.Min(1, point.Y / 512 - 1));
    }

    private void RenderRoof(object sender, EventArgs args) {
        TimeSpan now = ((RenderingEventArgs)args).RenderingTime;
        double dt = previousFrame.HasValue ? Math.Max(0, Math.Min(.1, (now - previousFrame.Value).TotalSeconds)) : 0;
        previousFrame = now;
        AdvanceRoof(dt);
    }

    private void AdvanceRoof(double dt) {
        double mix = 1 - Math.Exp(-dt / .18);
        cityTravel.X += (targetX - cityTravel.X) * mix;
        cityTravel.Y += (targetY - cityTravel.Y) * mix;
        swingTime = (swingTime + dt) % 5.6;
        signSwing.Angle = .40 * Math.Sin(swingTime * 2 * Math.PI / 5.6);
    }

    internal void CheckRoofMotion() {
        StopRoofMotion();
        AimRoof(new Point(1536, 1024));
        AdvanceRoof(.18);
        if (Math.Abs(cityTravel.X + 9 * (1 - Math.Exp(-1))) > .001)
            throw new Exception("City did not ease opposite the pointer.");
        for (int i = 0; i < 122; ++i) AdvanceRoof(.01);
        if (Math.Abs(signSwing.Angle - .40) > .001 || Math.Abs(cityTravel.Y + 5) > .01)
            throw new Exception("Rooftop swing or vertical travel changed.");
        AimRoof(new Point(0, 0));
        for (int i = 0; i < 180; ++i) AdvanceRoof(.01);
        if (Math.Abs(cityTravel.X - 9) > .01 || Math.Abs(cityTravel.Y - 5) > .01)
            throw new Exception("City did not travel to its opposite bound.");
        StopRoofMotion();
        if (cityTravel.X != 0 || cityTravel.Y != 0 || signSwing.Angle != 0 || roofAnimating)
            throw new Exception("Rooftop did not settle when suspended.");
    }

    private void SetImage(string name, string path) {
        Get<Image>(name).Source = ReadImage(path);
    }

    private static BitmapImage ReadImage(string path) {
        Files.NoReparse(path);if (!File.Exists(path)) throw new FileNotFoundException("Missing launcher artwork",path);
        BitmapImage source = new BitmapImage();
        source.BeginInit(); source.CacheOption = BitmapCacheOption.OnLoad; source.UriSource = new Uri(Path.GetFullPath(path)); source.EndInit(); source.Freeze();
        return source;
    }

    private void SetupChoices() {
        settingUp = true;
        ComboBox main = Get<ComboBox>("LanguageBox");
        foreach (string[] lang in UiText.Languages) main.Items.Add(LanguageItem(lang));
        ComboBox display = Get<ComboBox>("DisplayBox");
        display.Items.Add(new Choice(Display.Borderless, "modeBorderless"));
        display.Items.Add(new Choice(Display.Window, "modeWindow"));
        display.Items.Add(new Choice(Display.Original, "modeOriginal"));
        BackendState state = backend.Snapshot();
        SelectLanguage(main, UiText.Current);
        SetIndex(display, state.DisplayMode);
        ComboBox resolutions = Get<ComboBox>("ResolutionBox");
        foreach (string resolution in state.Resolutions ?? new string[0]) if (!resolutions.Items.Contains(resolution)) resolutions.Items.Add(resolution);
        if (state.Resolution != null) resolutions.SelectedItem = state.Resolution;
        settingUp = false;
        main.SelectionChanged += LanguageChanged;
        display.SelectionChanged += delegate { if (!settingUp && display.SelectedIndex >= 0) backend.UiSetDisplay(display.SelectedIndex); };
        resolutions.SelectionChanged += delegate { if (!settingUp && resolutions.SelectedItem != null) backend.UiSetResolution(resolutions.SelectedItem.ToString()); };
    }

    private ComboBoxItem LanguageItem(string[] lang) { return new ComboBoxItem { Content = lang[1], Tag = lang[0] }; }
    private void SelectLanguage(ComboBox box, string code) {
        foreach (ComboBoxItem item in box.Items) if (String.Equals(Convert.ToString(item.Tag), code, StringComparison.OrdinalIgnoreCase)) { box.SelectedItem = item; return; }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }
    private void SetIndex(ComboBox box, int index) { if (index >= 0 && index < box.Items.Count) box.SelectedIndex = index; }

    private void LanguageChanged(object sender, SelectionChangedEventArgs e) {
        if (settingUp) return;
        ComboBox box = sender as ComboBox; ComboBoxItem selected = box == null ? null : box.SelectedItem as ComboBoxItem;
        if (selected == null) return;
        SetLanguage(Convert.ToString(selected.Tag));
    }

    internal void SetLanguage(string code) {
        if (!UiText.Languages.Any(x => String.Equals(x[0], code, StringComparison.OrdinalIgnoreCase))) code = "en";
        settingUp = true;
        backend.UiSetLanguage(code);
        SelectLanguage(Get<ComboBox>("LanguageBox"), code);
        settingUp = false;
        ApplyTexts();Refresh();
    }

    private void ApplyTexts() {
        window.Title = UiText.Get("title");
        string readableTitle=UiText.Get("title");int titleSeparator=readableTitle.IndexOf('—');Get<TextBlock>("AccessibleSubtitle").Text=titleSeparator>=0?readableTitle.Substring(titleSeparator+1).Trim():readableTitle;
        SetActionLabel("HostLabel","comicHost");
        Get<TextBlock>("HostSub").Text = UiText.Get("comicHostSub");
        SetActionLabel("JoinLabel","comicJoin");
        Get<TextBlock>("JoinSub").Text = UiText.Get("comicJoinSub");
        Get<TextBlock>("GameStatus").Text = UiText.Get("comicGameReady");
        Get<TextBlock>("SteamStatus").Text = UiText.Get("comicSetup");
        Get<Button>("SettingsButton").ToolTip = UiText.Get("comicSettings");
        Get<TextBlock>("SettingsTitle").Text = UiText.Get("comicSettings");
        Get<TextBlock>("SessionPaperTitle").Text = UiText.Get("title");
        Get<TextBlock>("DisplayLabel").Text = UiText.Get("display");
        Get<TextBlock>("SteamLabel").Text = UiText.Get("steam");
        Get<Button>("SteamBrowse").Content = UiText.Get("choose");
        Get<Button>("VerifyButton").Content = UiText.Get("verify");
        Get<Button>("ImportButton").Content = UiText.Get("import");
        Get<Button>("SettingsDone").Content = UiText.Get("comicDone");
        Get<TextBlock>("JoinCardTitle").Text = UiText.Get("comicJoin");
        Get<TextBlock>("JoinCardHint").Text = UiText.Get("comicGuide");
        Get<Button>("JoinPaste").Content = UiText.Get("paste");
        Get<Button>("JoinCancel").Content = UiText.Get("comicCancel");
        Get<Button>("JoinSubmit").Content = UiText.Get("comicJoin");
        Get<Button>("CopyLobby").Content=UiText.Get("copy");Get<Button>("StopButton").Content=UiText.Get("stop");Get<TextBlock>("LobbyLabel").Text=UiText.Get("lobby");
        Get<System.Windows.Controls.Primitives.ToggleButton>("AccessibilityButton").ToolTip=UiText.Get("comicAccessibility");
        System.Windows.Automation.AutomationProperties.SetName(Get<System.Windows.Controls.Primitives.ToggleButton>("AccessibilityButton"),UiText.Get("comicAccessibility"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("SettingsButton"),UiText.Get("comicSettings"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("HostButton"),UiText.Get("comicHost"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("JoinButton"),UiText.Get("comicJoin"));
        System.Windows.Automation.AutomationProperties.SetName(Get<ComboBox>("LanguageBox"),UiText.Get("uiLanguage"));
        System.Windows.Automation.AutomationProperties.SetName(Get<TextBox>("LobbyText"),UiText.Get("lobby"));
        foreach(string name in new[]{"JoinCardClose","SettingsCardClose"})System.Windows.Automation.AutomationProperties.SetName(Get<Button>(name),UiText.Get("comicCancel"));
        ApplySettingsText();
        UpdateRoofLocaleText();
        settingUp=true;Get<ComboBox>("DisplayBox").Items.Refresh();settingUp=false;
    }

    private void ApplySettingsText() {
        SetTab("DisplayTab", "display", "hintDisplay");
        SetTab("SetupTab", "tabSetup", "guideSetup");
        Get<TabItem>("GuideTab").Header = UiText.Get("tabGuide");
        SetFieldHint("DisplayBox", "DisplayHint", "hintDisplay");
        SetFieldHint("ResolutionBox", "ResolutionHint", "hintResolution");
        SetFieldHint("SteamPathBox", "SteamHint", "guideSteam");
        Hint(Get<Button>("SteamBrowse"), UiText.Get("guideSteam"));
        Hint(Get<Button>("ImportButton"), UiText.Get("importDialog"));
        Hint(Get<Button>("VerifyButton"), UiText.Get("statusReady"));
        Hint(Get<Button>("SettingsDone"), UiText.Get("comicDone"));
        Hint(Get<Button>("SettingsCardClose"), UiText.Get("comicSettings") + " · " + UiText.Get("comicDone"));
        Get<TextBlock>("ResolutionLabel").Text = UiText.Get("resolution");
        Get<TextBlock>("FilesLabel").Text = UiText.Get("files");
        Get<TextBlock>("FilesHint").Text = UiText.Get("importDialog");
        Get<TextBlock>("GuideSetupTitle").Text = "1. " + UiText.Get("tabSetup");
        Get<TextBlock>("GuideSetupText").Text = UiText.Get("guideSetup") + Environment.NewLine + UiText.Get("guideSteam");
        Get<TextBlock>("GuideHostTitle").Text = "2. " + UiText.Get("comicHost");
        Get<TextBlock>("GuideHostText").Text = UiText.Get("guideHost");
        Get<TextBlock>("GuideJoinTitle").Text = "3. " + UiText.Get("comicJoin");
        Get<TextBlock>("GuideJoinText").Text = UiText.Get("guideJoin");
        Get<TextBlock>("GuideTroubleTitle").Text = UiText.Get("comicHelp");
        Get<TextBlock>("GuideTroubleText").Text = UiText.Get("guideTrouble");
        Get<TextBlock>("LogLabel").Text = UiText.Get("activityLog");
    }

    private void SetTab(string name, string title, string hint) {
        var tab = Get<TabItem>(name);
        tab.Header = UiText.Get(title);
        Hint(tab, UiText.Get(hint));
    }

    private void SetFieldHint(string field, string caption, string key) {
        string description = UiText.Get(key);
        Get<TextBlock>(caption).Text = description;
        Hint(Get<Control>(field), description);
    }

    private static void Hint(Control control, string description) {
        control.ToolTip = new ToolTip { Content = new TextBlock {
            Text = description, TextWrapping = TextWrapping.Wrap, MaxWidth = 350,
            FontFamily = new FontFamily("Segoe UI"), FontSize = 15
        }};
        System.Windows.Automation.AutomationProperties.SetHelpText(control, description);
        ToolTipService.SetShowOnDisabled(control, true);
    }

    private void SetActionLabel(string name, string key) {
        var label = Get<TextBlock>(name);
        label.Text = UiText.Get(key);
        label.FontFamily = new FontFamily(accessible ? "Segoe UI" : "Georgia");
        label.FontStyle = accessible ? FontStyles.Normal : FontStyles.Italic;
        label.FontSize = 39;
    }
    private void UpdateJoinEnabled(){ulong value;string code=Get<TextBox>("JoinCodeBox").Text;Get<Button>("JoinSubmit").IsEnabled=code.Length<=20&&UInt64.TryParse(code,System.Globalization.NumberStyles.None,System.Globalization.CultureInfo.InvariantCulture,out value)&&value>0;}
    private void WireActions() {
        Get<TextBox>("JoinCodeBox").TextChanged+=delegate{UpdateJoinEnabled();};UpdateJoinEnabled();
        Get<Button>("CopyLobby").Click+=delegate{string code=ReviewState(backend.Snapshot()).Lobby;if(!String.IsNullOrEmpty(code))Clipboard.SetText(code);};
        Get<Button>("HostButton").Click += async delegate { if (review) return; await SafeAction(delegate { return backend.UiHost(); }); };
        Get<Button>("JoinButton").Click += delegate { OpenCard("JoinCard"); Get<TextBox>("JoinCodeBox").Focus(); };
        Get<Button>("SettingsButton").Click += delegate { SyncSettings(backend.Snapshot()); OpenCard("SettingsCard"); };
        Get<Button>("StopButton").Click += async delegate { if (!review) await SafeAction(delegate { return backend.UiStop(); }); };
        Get<Button>("VerifyButton").Click += async delegate { if (!review) await SafeAction(delegate { return backend.UiVerify(); }); };
        Get<Button>("ImportButton").Click += delegate { if (!review) backend.UiImport(); };
        Get<Button>("SteamBrowse").Click += delegate { if (!review) backend.UiBrowseSteam(); };
        Get<Button>("JoinSubmit").Click += async delegate { await SubmitJoin(); };
        Get<TextBox>("JoinCodeBox").KeyDown += async delegate(object sender, KeyEventArgs e) { if (e.Key == Key.Enter) { e.Handled = true; await SubmitJoin(); } };
        Get<Button>("JoinPaste").Click += delegate {
            try { if (Clipboard.ContainsText()) Get<TextBox>("JoinCodeBox").Text = Clipboard.GetText().Trim(); } catch { }
        };
        Get<Button>("JoinCancel").Click += delegate { CloseCard(); };
        Get<Button>("JoinCardClose").Click += delegate { CloseCard(); };
        Get<Button>("SettingsDone").Click += delegate { CloseCard(); };
        Get<Button>("SettingsCardClose").Click += delegate { CloseCard(); };
        Get<Button>("MinButton").Click += delegate { window.WindowState = WindowState.Minimized; };
        Get<Button>("CloseButton").Click += delegate { window.Close(); };
        Get<Border>("DragRegion").MouseLeftButtonDown += delegate(object sender, MouseButtonEventArgs e) {
            if(e.ClickCount==2){e.Handled=true;return;}
            if (e.ButtonState == MouseButtonState.Pressed) { try { window.DragMove(); } catch (InvalidOperationException) { } }
        };
    }

    private void OpenCard(string card) {
        if(Get<Grid>("Overlay").Visibility!=Visibility.Visible)cardReturnFocus=Keyboard.FocusedElement;
        Get<Border>("JoinCard").Visibility = Visibility.Collapsed;
        Get<Border>("SettingsCard").Visibility = Visibility.Collapsed;
        Get<Grid>("Overlay").Visibility = Visibility.Visible;
        if (card == "SettingsCard") Get<TabControl>("SettingsTabs").SelectedIndex = 0;
        Get<Border>(card).Visibility = Visibility.Visible;
        Get<Canvas>("Stage").Children.Cast<UIElement>().Where(x=>x!=Get<Grid>("Overlay")).ToList().ForEach(x=>x.IsEnabled=false);
        if(card=="SettingsCard")Get<TabItem>("DisplayTab").Focus();
    }
    private void CloseCard() { Get<Grid>("Overlay").Visibility = Visibility.Collapsed;Get<Canvas>("Stage").Children.Cast<UIElement>().Where(x=>x!=Get<Grid>("Overlay")).ToList().ForEach(x=>x.IsEnabled=true);Refresh();if(cardReturnFocus!=null)Keyboard.Focus(cardReturnFocus);else Get<Button>("JoinButton").Focus(); }

    private async Task SubmitJoin() {
        string code = Get<TextBox>("JoinCodeBox").Text.Trim();
        ulong parsed;if(code.Length<1||code.Length>20||!UInt64.TryParse(code,System.Globalization.NumberStyles.None,System.Globalization.CultureInfo.InvariantCulture,out parsed)||parsed==0)return;
        if(review){CloseCard();return;}
        CloseCard();
        await SafeAction(delegate { return backend.UiJoin(code); });
    }

    private async Task SafeAction(Func<Task> action) {
        try { await action(); } catch (Exception ex) { Get<TextBlock>("ActivityText").Text = "REJECTED: " + ex.Message; }
        Refresh();
    }

    private void WindowKeyDown(object sender, KeyEventArgs e) {
        if(review&&e.Key>=Key.F1&&e.Key<=Key.F9){string[] states={"ready","missing","hosting","joining","error","settings","join","small","max"};ConfigurePreview(states[(int)e.Key-(int)Key.F1]);if(e.Key==Key.F8){window.Width=window.MinWidth;window.Height=window.MinHeight;}if(e.Key==Key.F9){window.Width=window.MaxWidth;window.Height=window.MaxHeight;}e.Handled=true;return;}
        if (e.Key == Key.Escape && Get<Grid>("Overlay").Visibility == Visibility.Visible) { CloseCard(); e.Handled = true; }
    }

    private void WindowClosing(object sender, System.ComponentModel.CancelEventArgs e) {
        if (allowClose) return;
        e.Cancel = true;
        if (closing) return;
        closing = true;
        window.IsEnabled = false;
        CloseAsync();
    }

    private async void CloseAsync() {
        bool closed = false;
        try { closed = await backend.UiClose(); } catch { }
        if (closed) {
            allowClose = true; timer.Stop(); if(friendNotice!=null)friendNotice.Close(); backend.Dispose(); await window.Dispatcher.BeginInvoke(new Action(window.Close));
        } else {
            closing = false;
            window.IsEnabled = true;
            Get<TextBlock>("ActivityText").Text = "The session is still stopping. Try closing again shortly.";
        }
    }

    private void Refresh() {
        BackendState state;
        try { state = ReviewState(backend.Snapshot()); } catch (ObjectDisposedException) { return; }
        if (state == null) return;
        if(state.FriendJoinedSequence>seenFriendJoined){seenFriendJoined=state.FriendJoinedSequence;if(!review)ShowFriendNotice();}
        if(!state.Active&&friendNotice!=null)friendNotice.Close();
        Get<Button>("HostButton").IsEnabled = state.HostEnabled;
        Get<Button>("JoinButton").IsEnabled = state.JoinEnabled;
        Get<Button>("StopButton").Content = UiText.Get(state.VerifyingSession?"comicCancel":"stop");
        Get<Button>("StopButton").IsEnabled = state.StopEnabled;
        Get<Button>("ImportButton").IsEnabled = !review && state.SetupEnabled;
        Get<Button>("VerifyButton").IsEnabled = !review && state.SetupEnabled;
        Get<Button>("SteamBrowse").IsEnabled = !review && state.SetupEnabled;
        foreach(string name in new[]{"DisplayBox","ResolutionBox"})Get<ComboBox>(name).IsEnabled=state.SetupEnabled;
        Get<TextBlock>("ResolutionLabel").Visibility=Get<TextBlock>("ResolutionHint").Visibility=Get<ComboBox>("ResolutionBox").Visibility=state.DisplayMode==2?Visibility.Collapsed:Visibility.Visible;
        Get<Ellipse>("GameDot").Fill = Brush(state.GameReady ? "#258A59" : "#C78A26");
        Get<TextBlock>("GameStatus").Text = UiText.Get(state.GameReady ? "comicGameReady" : "comicGameSetup");
        Get<Ellipse>("SteamDot").Fill = Brush(state.SteamReady ? "#258A59" : "#C78A26");
        Get<TextBlock>("SteamStatus").Text = "Steam: " + UiText.Get(state.SteamReady ? "comicReady" : "comicSetup");
        bool liveStatus=accessible||UiText.Current!="en"||!state.GameReady||!state.SteamReady;
        Get<Image>("RoofStatusPlate").Visibility=liveStatus?Visibility.Visible:Visibility.Collapsed;
        Get<Border>("StatusLive").Visibility=liveStatus?Visibility.Visible:Visibility.Collapsed;
        System.Windows.Automation.AutomationProperties.SetHelpText(Get<Button>("HostButton"), UiText.Get("comicHost") + ". " + UiText.Get(state.GameReady ? "comicGameReady" : "comicGameSetup"));
        System.Windows.Automation.AutomationProperties.SetHelpText(Get<Button>("JoinButton"), UiText.Get("comicJoin") + ". " + UiText.Get(state.SteamReady ? "comicReady" : "comicSetup"));
        System.Windows.Automation.AutomationProperties.SetHelpText(Get<ComboBox>("LanguageBox"), UiText.Get("uiLanguage") + ": " + UiText.Current);
        bool showSession=state.Active||(!String.IsNullOrEmpty(state.Activity)&&state.Activity.StartsWith("REJECTED:",StringComparison.Ordinal));
        Get<Border>("SessionBar").Visibility=showSession?Visibility.Visible:Visibility.Collapsed;
        Get<Button>("CopyLobby").Visibility=String.IsNullOrEmpty(state.Lobby)?Visibility.Collapsed:Visibility.Visible;
        TextBlock activity = Get<TextBlock>("ActivityText");
        string text = String.IsNullOrEmpty(state.Activity) ? UiText.Get("comicGuide") : state.Activity;
        if (activity.Text != text) activity.Text = text;
        TextBox lobby = Get<TextBox>("LobbyText"); string code=state.Lobby??"";
        if (lobby.Text != code) lobby.Text = code;
        Get<TextBlock>("LobbyLabel").Visibility=lobby.Visibility=String.IsNullOrEmpty(code)?Visibility.Collapsed:Visibility.Visible;
        bool sessionActions=state.StopEnabled||!String.IsNullOrEmpty(code);
        Get<Button>("StopButton").Visibility=state.StopEnabled?Visibility.Visible:Visibility.Collapsed;
        Get<Grid>("LobbyRow").Visibility=sessionActions?Visibility.Visible:Visibility.Collapsed;
        ((Grid)Get<Border>("SessionBar").Child).RowDefinitions[1].Height=new GridLength(sessionActions?56:0);
        if(Get<Grid>("Overlay").Visibility==Visibility.Visible)Get<Canvas>("Stage").Children.Cast<UIElement>().Where(x=>x!=Get<Grid>("Overlay")).ToList().ForEach(x=>x.IsEnabled=false);
        TextBox steam = Get<TextBox>("SteamPathBox"); if (steam.Text != (state.SteamPath ?? "")) steam.Text = state.SteamPath ?? "";
        TextBox log = Get<TextBox>("LogBox"); string latestLog = state.Log ?? "";
        if (log.Text != latestLog && !log.IsKeyboardFocused) { log.Text = latestLog; log.ScrollToEnd(); }
        if (!review && !Get<Border>("SettingsCard").IsKeyboardFocusWithin) SyncSettings(state);
    }

    private void SyncSettings(BackendState state) {
        if (state == null) return;
        settingUp = true;
        SetIndex(Get<ComboBox>("DisplayBox"), state.DisplayMode);
        ComboBox resolutions = Get<ComboBox>("ResolutionBox");
        foreach (string resolution in state.Resolutions ?? new string[0]) if (!resolutions.Items.Contains(resolution)) resolutions.Items.Add(resolution);
        if (state.Resolution != null && resolutions.Items.Contains(state.Resolution)) resolutions.SelectedItem = state.Resolution;
        settingUp = false;
    }

    private void CheckRooftopGeometry(){
        var roof=Get<Canvas>("RoofCanvas");
        if(roof.Width!=1536||roof.Height!=1024||roof.ClipToBounds!=true)throw new Exception("Rooftop artboard dimensions changed");
        CheckRoofLayer("RoofCity",0,0,1536,1024);
        CheckRoofLayer("RoofTitle",0,0,1536,1024);
        CheckRoofLayer("RoofFront",0,0,1536,1024);
        CheckRoofHit("HostButton",65,493,437,249);
        CheckRoofHit("JoinButton",1065,536,461,247);
        CheckRoofHit("LanguageBox",34,915,175,64);
        CheckRoofHit("SettingsButton",1358,915,65,65);
        CheckRoofHit("AccessibilityButton",1434,914,67,66);
        if(window.FindName("MaxButton")!=null)throw new Exception("Obsolete toolbar command is still present");
    }
    private void CheckRoofLayer(string name,double left,double top,double width,double height){
        var image=Get<Image>(name);var roof=Get<Canvas>("RoofCanvas");
        if(!ReferenceEquals(image.Parent,roof)||Canvas.GetLeft(image)!=left||Canvas.GetTop(image)!=top||image.Width!=width||image.Height!=height||image.Source==null||image.Source.Width!=width||image.Source.Height!=height||image.IsHitTestVisible)
            throw new Exception("Rooftop layer geometry/source changed: "+name);
    }
    private void CheckRoofHit(string name,double left,double top,double width,double height){
        var item=Get<FrameworkElement>(name);
        if(!ReferenceEquals(item.Parent,Get<Canvas>("RoofCanvas"))||Canvas.GetLeft(item)!=left||Canvas.GetTop(item)!=top||item.Width!=width||item.Height!=height)
            throw new Exception("Rooftop hit target changed: "+name);
    }

    internal void CheckUi(){
        CheckRooftopGeometry();backend.CheckFriendJoinedSignal();
        foreach(var language in UiText.Languages){SetLanguage(language[0]);var notice=CreateFriendNotice();notice.Measure(new Size(420,112));notice.Arrange(new Rect(0,0,420,112));var board=(Border)notice.Content;var text=(TextBlock)board.Child;
            if(notice.ShowActivated||!notice.Topmost||notice.ShowInTaskbar||notice.IsHitTestVisible||text.Text!=UiText.Get("friendJoined")||text.DesiredSize.Height>112-38)throw new Exception("Joined notification unreadable or intrusive "+language[0]);
            IntPtr before=GetForegroundWindow();notice.Opacity=0;notice.Show();notice.UpdateLayout();var handle=new System.Windows.Interop.WindowInteropHelper(notice).Handle;
            if(GetForegroundWindow()!=before||(NoticeGetStyle(handle,-20).ToInt64()&0x08000020L)!=0x08000020L)throw new Exception("Joined notification takes focus or input");notice.Close();}
        Console.WriteLine("HOST_NOTICE_PASS 11-locales/host-only/deduplicated/reconnect/cancel/no-activation");
        CheckReviewMatrix();
        foreach(var language in UiText.Languages){SetLanguage(language[0]);window.UpdateLayout();foreach(string name in new[]{"HostLabel","JoinLabel"}){var label=Get<TextBlock>(name);if(String.IsNullOrEmpty(label.Text)||label.FontSize<15)throw new Exception("Invalid localized action label "+language[0]);}CheckAccessibilityAndSession();}
        SetLanguage("en");Get<Button>("JoinButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Grid>("Overlay").Visibility!=Visibility.Visible)throw new Exception("Join did not open");
        foreach(string invalid in new[]{"","0","-1","123abc"," 2","18446744073709551616"}){Get<TextBox>("JoinCodeBox").Text=invalid;if(Get<Button>("JoinSubmit").IsEnabled)throw new Exception("Accepted invalid lobby code");}
        Get<TextBox>("JoinCodeBox").Text="123456";if(!Get<Button>("JoinSubmit").IsEnabled)throw new Exception("Valid lobby disabled");
        Get<Button>("JoinCancel").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Grid>("Overlay").Visibility!=Visibility.Collapsed||backend.Snapshot().Active)throw new Exception("Cancel side effect");
        Get<Button>("SettingsButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Border>("SettingsCard").Visibility!=Visibility.Visible)throw new Exception("Settings did not open");Get<TabItem>("GuideTab").IsSelected=true;window.UpdateLayout();if(!Get<TextBox>("LogBox").IsVisible||Get<TextBlock>("GuideHostText").Text.Length==0)throw new Exception("Help and log missing");Get<Button>("SettingsDone").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if(!Get<Button>("HostButton").Focusable||!Get<Button>("JoinButton").Focusable)throw new Exception("Keyboard actions disabled");foreach(string name in new[]{"RoofCity","RoofTitle","RoofFront"})if(Get<Image>(name).Source==null)throw new Exception("Missing rooftop art");
    }

    private static SolidColorBrush Brush(string color) { SolidColorBrush brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color)); brush.Freeze(); return brush; }
}
}
