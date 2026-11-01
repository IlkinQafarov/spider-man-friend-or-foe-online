using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Markup;
using System.Windows.Threading;
using Ellipse = System.Windows.Shapes.Ellipse;
using ShapePath = System.Windows.Shapes.Path;
using Polygon = System.Windows.Shapes.Polygon;

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
        shell.ConfigurePreview(flags);
        window.Show();
        window.UpdateLayout();
        bool toolbarOnly=(flags??"").Contains("toolbar");
        // Capture the laid-out root, including uniform-fit gutters and border, without stretching Stage.
        FrameworkElement content = toolbarOnly ? (FrameworkElement)window.FindName("ReferenceToolbarCanvas") : (FrameworkElement)window.Content;
        content.UpdateLayout();
        double scale=flags.Contains("dpi200")?2:flags.Contains("dpi150")?1.5:1;
        double logicalWidth=toolbarOnly?845:content.ActualWidth,logicalHeight=toolbarOnly?126:content.ActualHeight;
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

    public static int Selftest(){Application app=Application.Current??new Application();app.ShutdownMode=ShutdownMode.OnExplicitShutdown;using(var backend=new LauncherForm(true)){Window window=LoadWindow();window.ShowInTaskbar=false;window.ShowActivated=false;window.Opacity=0;window.Left=-12000;window.Top=-12000;ComicWindow shell=new ComicWindow(window,backend,true);shell.Initialize();window.Show();window.UpdateLayout();WindowFrame.Check(window);shell.CheckUi();shell.PreparePreviewClose();window.Close();}Console.WriteLine("WPF_UI_SELFTEST_PASS reference-toolbar/11-locales/accessibility-toggle/20-digit-session-fit/rectangular-panels/labels/join-validation/cancel/settings/keyboard/three-backgrounds/title/no-account-no-game");return 0;}

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
    private bool settingUp, allowClose, closing, helpFromSettings;
    private IInputElement cardReturnFocus;
    private int seenFriendJoined;
    private Window friendNotice;
    [System.Runtime.InteropServices.DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] static extern IntPtr NoticeGetStyle(IntPtr hwnd,int index);
    [System.Runtime.InteropServices.DllImport("user32.dll",EntryPoint="SetWindowLongPtrW")] static extern IntPtr NoticeSetStyle(IntPtr hwnd,int index,IntPtr value);
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    private static Window CreateFriendNotice(){
        var notice=new Window{Width=420,Height=112,WindowStyle=WindowStyle.None,ResizeMode=ResizeMode.NoResize,
            ShowInTaskbar=false,ShowActivated=false,Topmost=true,IsHitTestVisible=false,Focusable=false,
            Background=Brushes.White,Content=new Border{Background=Brushes.White,BorderBrush=Brushes.Black,BorderThickness=new Thickness(3),Padding=new Thickness(22,16,22,16),
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
        SetupChoices();
        WireActions();
        ApplyTexts();
        Refresh();
        InitializeAccessibility();
        timer.Start();
        window.PreviewKeyDown += WindowKeyDown;
        window.Closing += WindowClosing;
        window.Closed += delegate { timer.Stop(); };
    }

    internal void PreparePreviewClose(){allowClose=true;timer.Stop();}

    private void LoadArt() {
        string root = Files.Root;
        SetImage("HeaderArt", Path.Combine(root, "launcher", "assets", "backgrounds", "header.png"));
        SetImage("HostArt", Path.Combine(root, "launcher", "assets", "backgrounds", "host.png"));
        SetImage("JoinArt", Path.Combine(root, "launcher", "assets", "backgrounds", "join.png"));
        SetImage("TitleArt", Path.Combine(root, "launcher", "assets", "foreground", "title.png"));
        SetIcon("HostIcon",Path.Combine(root,"launcher","assets","foreground","host-icon.png"),new Int32Rect(227,40,1096,931));
        SetIcon("JoinIcon",Path.Combine(root,"launcher","assets","foreground","join-icon.png"),new Int32Rect(129,56,1509,776));
    }

    private void SetIcon(string name,string path,Int32Rect visibleBounds){SetImage(name,path);var image=Get<Image>(name);var cropped=new CroppedBitmap((BitmapSource)image.Source,visibleBounds);cropped.Freeze();image.Source=cropped;}
    private void SetImage(string name, string path) {
        Image image = Get<Image>(name);
        Files.NoReparse(path);if (!File.Exists(path)) throw new FileNotFoundException("Missing launcher artwork",path);
        BitmapImage source = new BitmapImage();
        source.BeginInit(); source.CacheOption = BitmapCacheOption.OnLoad; source.UriSource = new Uri(Path.GetFullPath(path)); source.EndInit(); source.Freeze();
        image.Source = source;
    }

    private void SetupChoices() {
        settingUp = true;
        ComboBox main = Get<ComboBox>("LanguageBox"), settings = Get<ComboBox>("SettingsLanguageBox");
        foreach (string[] lang in UiText.Languages) { main.Items.Add(LanguageItem(lang)); settings.Items.Add(LanguageItem(lang)); }
        Get<ComboBox>("GameLanguageBox").Items.Add(new Choice("English", "gameEnglish"));
        Get<ComboBox>("GameLanguageBox").Items.Add(new Choice("Russian", "gameRussian"));
        ComboBox display = Get<ComboBox>("DisplayBox");
        display.Items.Add(new Choice(Display.Borderless, "modeBorderless"));
        display.Items.Add(new Choice(Display.Window, "modeWindow"));
        display.Items.Add(new Choice(Display.Original, "modeOriginal"));
        BackendState state = backend.Snapshot();
        SelectLanguage(main, UiText.Current); SelectLanguage(settings, UiText.Current);
        SetIndex(Get<ComboBox>("GameLanguageBox"), state.GameLanguage);
        SetIndex(display, state.DisplayMode);
        ComboBox resolutions = Get<ComboBox>("ResolutionBox");
        foreach (string resolution in state.Resolutions ?? new string[0]) if (!resolutions.Items.Contains(resolution)) resolutions.Items.Add(resolution);
        if (state.Resolution != null) resolutions.SelectedItem = state.Resolution;
        settingUp = false;
        main.SelectionChanged += LanguageChanged;
        settings.SelectionChanged += LanguageChanged;
        Get<ComboBox>("GameLanguageBox").SelectionChanged += delegate {
            if (!settingUp && Get<ComboBox>("GameLanguageBox").SelectedIndex >= 0) backend.UiSetGameLanguage(Get<ComboBox>("GameLanguageBox").SelectedIndex);
        };
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
        SelectLanguage(Get<ComboBox>("SettingsLanguageBox"), code);
        settingUp = false;
        ApplyTexts();Refresh();
    }

    private void ApplyTexts() {
        window.Title = UiText.Get("title");
        Get<TextBlock>("Tagline").Text = UiText.Get("comicTagline");
        string readableTitle=UiText.Get("title");int titleSeparator=readableTitle.IndexOf('—');Get<TextBlock>("AccessibleSubtitle").Text=titleSeparator>=0?readableTitle.Substring(titleSeparator+1).Trim():readableTitle;
        SetActionLabel("HostLabel","comicHost");
        Get<TextBlock>("HostSub").Text = UiText.Get("comicHostSub");
        SetActionLabel("JoinLabel","comicJoin");
        Get<TextBlock>("JoinSub").Text = UiText.Get("comicJoinSub");
        Get<TextBlock>("GameStatus").Text = UiText.Get("comicGameReady");
        Get<TextBlock>("SteamStatus").Text = UiText.Get("comicSetup");
        Get<TextBlock>("HelpText").Text = "ⓘ  " + UiText.Get("comicHelp");
        Get<Button>("SettingsButton").ToolTip = UiText.Get("comicSettings");
        Get<ComboBox>("SettingsLanguageBox").ToolTip = UiText.Get("uiLanguage");
        Get<TextBlock>("SettingsTitle").Text = UiText.Get("comicSettings");
        Get<TextBlock>("UiLanguageLabel").Text = UiText.Get("uiLanguage");
        Get<TextBlock>("GameLanguageLabel").Text = UiText.Get("gameLanguage");
        Get<TextBlock>("DisplayLabel").Text = UiText.Get("display");
        Get<TextBlock>("SteamLabel").Text = UiText.Get("steam");
        Get<TextBlock>("LogLabel").Text = UiText.Get("comicDetails");
        Get<Button>("SteamBrowse").Content = UiText.Get("choose");
        Get<Button>("VerifyButton").Content = UiText.Get("verify");
        Get<Button>("ImportButton").Content = UiText.Get("import");
        Get<Button>("SettingsDone").Content = UiText.Get("comicDone");
        Get<TextBlock>("JoinCardTitle").Text = UiText.Get("comicJoin");
        Get<TextBlock>("JoinCardHint").Text = UiText.Get("comicGuide");
        Get<Button>("JoinPaste").Content = UiText.Get("paste");
        Get<Button>("JoinCancel").Content = UiText.Get("comicCancel");
        Get<Button>("JoinSubmit").Content = UiText.Get("comicJoin").ToUpperInvariant();
        Get<TextBlock>("HelpCardTitle").Text = UiText.Get("comicHelp");
        Get<TextBlock>("HelpCardBody").Text = UiText.Get("comicGuide") + Environment.NewLine + Environment.NewLine + UiText.Get("partnerWait");
        Get<Button>("HelpDone").Content = "OK";
        Get<Button>("CopyLobby").Content=UiText.Get("copy");Get<Button>("StopButton").Content=UiText.Get("stop");Get<TextBlock>("LobbyLabel").Text=UiText.Get("lobby");
        Get<System.Windows.Controls.Primitives.ToggleButton>("AccessibilityButton").ToolTip=UiText.Get("comicAccessibility");
        System.Windows.Automation.AutomationProperties.SetName(Get<System.Windows.Controls.Primitives.ToggleButton>("AccessibilityButton"),UiText.Get("comicAccessibility"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("SettingsButton"),UiText.Get("comicSettings"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("HostButton"),UiText.Get("comicHost"));
        System.Windows.Automation.AutomationProperties.SetName(Get<Button>("JoinButton"),UiText.Get("comicJoin"));
        System.Windows.Automation.AutomationProperties.SetName(Get<ComboBox>("LanguageBox"),UiText.Get("uiLanguage"));
        System.Windows.Automation.AutomationProperties.SetName(Get<TextBox>("LobbyText"),UiText.Get("lobby"));
        foreach(string name in new[]{"JoinCardClose","SettingsCardClose","HelpCardClose"})System.Windows.Automation.AutomationProperties.SetName(Get<Button>(name),UiText.Get("comicCancel"));
        UpdateLanguagePresentation();
        settingUp=true;foreach (ComboBox box in new[] { Get<ComboBox>("GameLanguageBox"), Get<ComboBox>("DisplayBox") }) box.Items.Refresh();settingUp=false;
    }

    private void SetActionLabel(string name,string key){var label=Get<TextBlock>(name);label.Text=UiText.Get(key).ToUpper(UiText.Current=="tr"?new System.Globalization.CultureInfo("tr-TR"):System.Globalization.CultureInfo.InvariantCulture);label.FontFamily=new FontFamily(accessible?"Segoe UI":UiText.Current=="zh"?"Microsoft YaHei UI":"Impact");label.FontStyle=accessible?FontStyles.Normal:FontStyles.Italic;label.FontWeight=accessible?FontWeights.SemiBold:FontWeights.Bold;label.FontSize=110;label.TextTrimming=TextTrimming.None;label.TextWrapping=TextWrapping.NoWrap;}
    private void UpdateJoinEnabled(){ulong value;string code=Get<TextBox>("JoinCodeBox").Text;Get<Button>("JoinSubmit").IsEnabled=code.Length<=20&&UInt64.TryParse(code,System.Globalization.NumberStyles.None,System.Globalization.CultureInfo.InvariantCulture,out value)&&value>0;}
    private void WireActions() {
        Get<TextBox>("JoinCodeBox").TextChanged+=delegate{UpdateJoinEnabled();};UpdateJoinEnabled();
        Get<Button>("CopyLobby").Click+=delegate{string code=ReviewState(backend.Snapshot()).Lobby;if(!String.IsNullOrEmpty(code))Clipboard.SetText(code);};
        Get<Button>("HostButton").Click += async delegate { if (review) return; await SafeAction(delegate { return backend.UiHost(); }); };
        Get<Button>("JoinButton").Click += delegate { OpenCard("JoinCard"); Get<TextBox>("JoinCodeBox").Focus(); };
        Get<Button>("SettingsButton").Click += delegate { SyncSettings(backend.Snapshot()); OpenCard("SettingsCard"); };
        Get<Button>("HelpButton").Click += delegate { helpFromSettings=Get<Border>("SettingsCard").Visibility==Visibility.Visible;OpenCard("HelpCard"); };
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
        Get<Button>("HelpDone").Click += delegate { CloseHelp(); };
        Get<Button>("HelpCardClose").Click += delegate { CloseHelp(); };
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
        Get<Border>("HelpCard").Visibility = Visibility.Collapsed;
        Get<Grid>("Overlay").Visibility = Visibility.Visible;
        Get<Border>(card).Visibility = Visibility.Visible;
        Get<Canvas>("Stage").Children.Cast<UIElement>().Where(x=>x!=Get<Grid>("Overlay")).ToList().ForEach(x=>x.IsEnabled=false);
        if(card=="SettingsCard")Get<ComboBox>("SettingsLanguageBox").Focus();
        if(card=="HelpCard")Get<Button>("HelpDone").Focus();
    }
    private void CloseHelp(){if(helpFromSettings){helpFromSettings=false;OpenCard("SettingsCard");}else CloseCard();}
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
        if (e.Key == Key.Escape && Get<Grid>("Overlay").Visibility == Visibility.Visible) { if(Get<Border>("HelpCard").Visibility==Visibility.Visible)CloseHelp();else CloseCard(); e.Handled = true; }
    }

    private void WindowClosing(object sender, System.ComponentModel.CancelEventArgs e) {
        if (allowClose) return;
        e.Cancel = true;
        if (closing) return;
        closing = true;
        CloseAsync();
    }

    private async void CloseAsync() {
        bool closed = false;
        try { closed = await backend.UiClose(); } catch { }
        if (closed) {
            allowClose = true; timer.Stop(); if(friendNotice!=null)friendNotice.Close(); backend.Dispose(); await window.Dispatcher.BeginInvoke(new Action(window.Close));
        } else {
            closing = false;
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
        Get<Button>("StopButton").IsEnabled = state.StopEnabled;
        Get<Button>("ImportButton").IsEnabled = !review && state.SetupEnabled;
        Get<Button>("VerifyButton").IsEnabled = !review && state.SetupEnabled;
        Get<Button>("SteamBrowse").IsEnabled = !review && state.SetupEnabled;
        foreach(string name in new[]{"GameLanguageBox","DisplayBox","ResolutionBox"})Get<ComboBox>(name).IsEnabled=state.SetupEnabled;
        Get<ComboBox>("ResolutionBox").Visibility=state.DisplayMode==2?Visibility.Collapsed:Visibility.Visible;
        Get<Ellipse>("GameDot").Fill = Brush(state.GameReady ? "#258A59" : "#C78A26");
        Get<ShapePath>("GameCheck").Data = Geometry.Parse(state.GameReady?"M4,14.5 L11,21.5 L24,6.5":"M14,4 L14,16 M14,23 L14,24");
        Get<TextBlock>("GameStatus").Text = UiText.Get(state.GameReady ? "comicGameReady" : "comicGameSetup");
        Get<Ellipse>("SteamDot").Fill = Brush(state.SteamReady ? "#258A59" : "#C78A26");
        Get<ShapePath>("SteamMark").Data = Geometry.Parse(state.SteamReady?"M4,14.5 L11,21.5 L24,6.5":"M14,4 L14,16 M14,23 L14,24");
        Get<TextBlock>("SteamStatus").Text = "Steam: " + UiText.Get(state.SteamReady ? "comicReady" : "comicSetup");
        bool showSession=state.Active||(!String.IsNullOrEmpty(state.Activity)&&state.Activity.StartsWith("REJECTED:",StringComparison.Ordinal));
        Get<Border>("SessionBar").Visibility=showSession?Visibility.Visible:Visibility.Collapsed;LayoutSession(showSession);
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
        SetIndex(Get<ComboBox>("GameLanguageBox"), state.GameLanguage);
        SetIndex(Get<ComboBox>("DisplayBox"), state.DisplayMode);
        ComboBox resolutions = Get<ComboBox>("ResolutionBox");
        foreach (string resolution in state.Resolutions ?? new string[0]) if (!resolutions.Items.Contains(resolution)) resolutions.Items.Add(resolution);
        if (state.Resolution != null && resolutions.Items.Contains(state.Resolution)) resolutions.SelectedItem = state.Resolution;
        settingUp = false;
    }

    private void CheckTicketFit(string prefix){
        var foreground=Get<Canvas>(prefix+"Foreground");var ticket=Get<Polygon>(prefix+"Ticket");var group=Get<Canvas>(prefix+"TicketText");
        var transform=group.TransformToAncestor(foreground);var geometry=new StreamGeometry();using(var context=geometry.Open()){context.BeginFigure(ticket.Points[0],true,true);context.PolyLineTo(ticket.Points.Skip(1).ToArray(),true,false);}
        foreach(Point corner in new[]{new Point(0,0),new Point(group.Width,0),new Point(0,group.Height),new Point(group.Width,group.Height)})if(!geometry.FillContains(transform.Transform(corner)))throw new Exception(prefix+" ticket text leaves its paper frame in "+UiText.Current);
        foreach(string suffix in new[]{"TitleFit","SubFit"}){var view=Get<Viewbox>(prefix+suffix);var text=(TextBlock)view.Child;if(text.ActualWidth<=0||text.ActualHeight<=0||view.Visibility!=Visibility.Visible)throw new Exception("Missing ticket text "+UiText.Current);}
    }

    private void CheckReferenceToolbar(){
        var canvas=Get<Canvas>("ReferenceToolbarCanvas");var language=Get<ComboBox>("LanguageBox");var settings=Get<Button>("SettingsButton");var group=Get<Border>("ReferenceWindowGroup");
        if(canvas.Width!=845||canvas.Height!=126||language.Width!=398||language.Height!=116||Canvas.GetLeft(settings)-(Canvas.GetLeft(language)+language.Width)!=30||Canvas.GetLeft(group)-(Canvas.GetLeft(settings)+settings.Width)!=31||group.BorderThickness.Left!=6||group.Width!=254||window.FindName("MaxButton")!=null)throw new Exception("Reference toolbar proportions or gaps changed");
    }

    private void CheckStatusGlyphs(){
        foreach(string name in new[]{"GameCheck","SteamMark"}){var path=Get<ShapePath>(name);var canvas=(Canvas)path.Parent;var grid=(Grid)((Viewbox)canvas.Parent).Parent;var saved=path.Data;
            foreach(string glyph in new[]{"M14,4 L14,16 M14,23 L14,24","M4,14.5 L11,21.5 L24,6.5"}){path.Data=Geometry.Parse(glyph);window.UpdateLayout();Rect bounds=path.TransformToAncestor(grid).TransformBounds(path.Data.GetRenderBounds(new Pen(Brushes.White,path.StrokeThickness){StartLineCap=path.StrokeStartLineCap,EndLineCap=path.StrokeEndLineCap,LineJoin=path.StrokeLineJoin}));
            if(Math.Abs((bounds.Left+bounds.Right)/2-grid.ActualWidth/2)>0.6||Math.Abs((bounds.Top+bounds.Bottom)/2-grid.ActualHeight/2)>0.6)throw new Exception("Status glyph is not centered: "+name);}path.Data=saved;
        }
    }

    internal void CheckUi(){
        CheckReferenceToolbar();CheckStatusGlyphs();backend.CheckFriendJoinedSignal();
        foreach(var language in UiText.Languages){SetLanguage(language[0]);var notice=CreateFriendNotice();notice.Measure(new Size(420,112));notice.Arrange(new Rect(0,0,420,112));var board=(Border)notice.Content;var text=(TextBlock)board.Child;
            if(notice.ShowActivated||!notice.Topmost||notice.ShowInTaskbar||notice.IsHitTestVisible||text.Text!=UiText.Get("friendJoined")||text.DesiredSize.Height>112-38)throw new Exception("Joined notification unreadable or intrusive "+language[0]);
            IntPtr before=GetForegroundWindow();notice.Opacity=0;notice.Show();notice.UpdateLayout();var handle=new System.Windows.Interop.WindowInteropHelper(notice).Handle;
            if(GetForegroundWindow()!=before||(NoticeGetStyle(handle,-20).ToInt64()&0x08000020L)!=0x08000020L)throw new Exception("Joined notification takes focus or input");notice.Close();}
        Console.WriteLine("HOST_NOTICE_PASS 11-locales/host-only/deduplicated/reconnect/cancel/no-activation");
        CheckReviewMatrix();
        foreach(var language in UiText.Languages){SetLanguage(language[0]);window.UpdateLayout();foreach(string name in new[]{"HostLabel","JoinLabel"}){var label=Get<TextBlock>(name);if(String.IsNullOrEmpty(label.Text)||label.FontSize<15)throw new Exception("Invalid localized action label "+language[0]);}CheckTicketFit("Host");CheckTicketFit("Join");CheckAccessibilityAndSession();}
        SetLanguage("en");Get<Button>("JoinButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Grid>("Overlay").Visibility!=Visibility.Visible)throw new Exception("Join did not open");
        foreach(string invalid in new[]{"","0","-1","123abc"," 2","18446744073709551616"}){Get<TextBox>("JoinCodeBox").Text=invalid;if(Get<Button>("JoinSubmit").IsEnabled)throw new Exception("Accepted invalid lobby code");}
        Get<TextBox>("JoinCodeBox").Text="123456";if(!Get<Button>("JoinSubmit").IsEnabled)throw new Exception("Valid lobby disabled");
        Get<Button>("JoinCancel").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Grid>("Overlay").Visibility!=Visibility.Collapsed||backend.Snapshot().Active)throw new Exception("Cancel side effect");
        Get<Button>("SettingsButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Border>("SettingsCard").Visibility!=Visibility.Visible)throw new Exception("Settings did not open");Get<Button>("HelpButton").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Border>("HelpCard").Visibility!=Visibility.Visible)throw new Exception("Settings help missing");Get<Button>("HelpDone").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));if(Get<Border>("SettingsCard").Visibility!=Visibility.Visible)throw new Exception("Help did not return to settings");Get<Button>("SettingsDone").RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if(!Get<Button>("HostButton").Focusable||!Get<Button>("JoinButton").Focusable)throw new Exception("Keyboard actions disabled");foreach(string name in new[]{"HeaderArt","HostArt","JoinArt","TitleArt","HostIcon","JoinIcon"})if(Get<Image>(name).Source==null)throw new Exception("Missing layered art");
    }

    private static SolidColorBrush Brush(string color) { SolidColorBrush brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color)); brush.Freeze(); return brush; }
}
}
