using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using Polygon = System.Windows.Shapes.Polygon;

namespace FofPrivate {
internal sealed partial class ComicWindow {
    private bool accessible,previewSession;
    private string reviewState="";
    private readonly List<FontState> normalFonts=new List<FontState>();
    private sealed class FontState {
        private static readonly DependencyProperty[] Properties = {
            Control.FontFamilyProperty, Control.FontSizeProperty, Control.FontStyleProperty, Control.FontWeightProperty
        };
        private readonly DependencyObject target;
        private readonly object[] original = new object[Properties.Length];
        private readonly double normalSize;

        internal FontState(DependencyObject target) {
            this.target = target;
            normalSize = (double)target.GetValue(Control.FontSizeProperty);
            for (int i = 0; i < Properties.Length; ++i)
                original[i] = target.ReadLocalValue(Properties[i]);
        }

        internal void Apply() {
            target.SetValue(Control.FontFamilyProperty, new FontFamily("Segoe UI"));
            target.SetValue(Control.FontSizeProperty, Math.Max(20, normalSize));
            target.SetValue(Control.FontStyleProperty, FontStyles.Normal);
            target.SetValue(Control.FontWeightProperty, FontWeights.SemiBold);
        }

        internal void Restore() {
            // Styles and inherited fonts must regain precedence after the override.
            for (int i = 0; i < Properties.Length; ++i) {
                if (original[i] == DependencyProperty.UnsetValue) target.ClearValue(Properties[i]);
                else target.SetValue(Properties[i], original[i]);
            }
        }
    }
    private static IEnumerable<DependencyObject> LogicalElements(DependencyObject root){yield return root;foreach(object item in LogicalTreeHelper.GetChildren(root)){var child=item as DependencyObject;if(child!=null)foreach(var element in LogicalElements(child))yield return element;}}
    private void InitializeAccessibility(){
        var toggle=Get<ToggleButton>("AccessibilityButton");toggle.Checked+=delegate{SetAccessibility(true);};toggle.Unchecked+=delegate{SetAccessibility(false);};
    }
    private void SetAccessibility(bool value){
        if(accessible==value)return;accessible=value;
        if (value) {
            // Snapshot the current, styled tree before changing any inherited values.
            foreach (var element in LogicalElements(window))
                if (element is Control || element is TextBlock) normalFonts.Add(new FontState(element));
            foreach (var font in normalFonts) font.Apply();
        } else {
            foreach (var font in normalFonts) font.Restore();
            normalFonts.Clear();
        }
        Get<Image>("RoofCity").Visibility=value?Visibility.Collapsed:Visibility.Visible;
        foreach(var image in paperPanels)image.Opacity=value?0.12:1;
        foreach(string name in new[]{"SessionBar","JoinCard","SettingsCard"}){
            if(value)Get<Border>(name).Background=Brushes.WhiteSmoke;
            else Get<Border>(name).ClearValue(Border.BackgroundProperty);
        }
        foreach(string name in new[]{"JoinCardTitle","SettingsTitle","SessionPaperTitle"}){
            if(value)Get<TextBlock>(name).Foreground=Brushes.Black;
            else Get<TextBlock>(name).ClearValue(TextBlock.ForegroundProperty);
        }
        Get<Image>("RoofTitle").Visibility=value?Visibility.Collapsed:Visibility.Visible;
        Get<Image>("RoofFront").Opacity=value?0.42:1.0;
        Get<TextBlock>("AccessibleTitle").Visibility=Get<TextBlock>("AccessibleSubtitle").Visibility=value?Visibility.Visible:Visibility.Collapsed;
        UpdateRoofLocaleText();
        UpdateRoofMotion();
        Refresh();
        Get<ToggleButton>("AccessibilityButton").IsChecked=value;
    }
    private void UpdateRoofLocaleText() {
        bool replace = accessible || UiText.Current != "en";
        Visibility show = replace ? Visibility.Visible : Visibility.Collapsed;
        Get<Image>("HoverHost").Source = hoverSigns[0,replace ? 1 : 0];
        Get<Image>("HoverJoin").Source = hoverSigns[1,replace ? 1 : 0];
        foreach (string role in new[]{"Host", "Join"}) {
            Get<Image>("Roof" + role + "Plate").Visibility = show;
            var paper = Get<Grid>(role + "Paper");
            paper.Visibility = show;
            paper.RenderTransform = new RotateTransform(accessible ? 0 : role == "Host" ? -8 : -3);
            paper.Background = accessible ? Brushes.WhiteSmoke : Brushes.Transparent;
        }
        Get<Image>("RoofLanguagePlate").Visibility = show;
        Get<Viewbox>("LanguageLive").Visibility = show;
        SetActionLabel("HostLabel","comicHost");
        SetActionLabel("JoinLabel","comicJoin");
        var selection = Get<ComboBox>("LanguageBox").SelectedItem as ComboBoxItem;
        Get<TextBlock>("LanguageText").Text = selection == null ? UiText.Current : Convert.ToString(selection.Content);
    }
    private BackendState ReviewState(BackendState state){
        if(!review||(!previewSession&&String.IsNullOrEmpty(reviewState)))return state;
        string mode=previewSession?"hosting":reviewState;
        state.GameReady=mode!="missing";state.SteamReady=true;state.HostEnabled=state.JoinEnabled=mode=="ready"||mode=="missing"||mode=="error";
        state.Active=mode=="hosting"||mode=="joining";state.StopEnabled=state.Active;state.SetupEnabled=!state.Active;
        state.Lobby=mode=="hosting"?UInt64.MaxValue.ToString(System.Globalization.CultureInfo.InvariantCulture):"";
        state.Activity=mode=="hosting"?UiText.Get("partnerWait"):mode=="joining"?UiText.Get("partnerLinkedJoin"):mode=="error"?"REJECTED: "+UiText.Get("steamNone"):"";
        return state;
    }
    internal void ConfigurePreview(string flags){
        if(!review)return;flags=flags??"";previewSession=flags.Contains("session");reviewState="";
        foreach(string state in new[]{"ready","missing","hosting","joining","error"})if(flags.Contains(state))reviewState=state;
        if(Get<Grid>("Overlay").Visibility==Visibility.Visible)CloseCard();
        SetAccessibility(flags.Contains("accessible"));Refresh();
        if(flags.Contains("settings")) {
            OpenCard("SettingsCard");
            Get<TabControl>("SettingsTabs").SelectedIndex = flags.Contains("guide") ? 2 : flags.Contains("setup") ? 1 : 0;
        }
        if(flags.Contains("join")&&!flags.Contains("joining"))OpenCard("JoinCard");
    }
    private void CheckAccessibilityAndSession(){
        // Test a twenty-digit lobby plus localized Copy/Stop labels at the full logical layout.
        previewSession=true;Refresh();window.UpdateLayout();
        var session=Get<Border>("SessionBar");
        foreach(string name in new[]{"ActivityText","LobbyText","CopyLobby","StopButton"}){
            var element=Get<FrameworkElement>(name);Rect bounds=element.TransformToAncestor(session).TransformBounds(new Rect(0,0,element.ActualWidth,element.ActualHeight));
            if(bounds.Left<0||bounds.Top<0||bounds.Right>session.ActualWidth||bounds.Bottom>session.ActualHeight)throw new Exception("Session content outside rectangle: "+name+" "+UiText.Current);
        }
        var code=Get<TextBox>("LobbyText");var text=new FormattedText(code.Text,System.Globalization.CultureInfo.InvariantCulture,FlowDirection.LeftToRight,new Typeface(code.FontFamily,code.FontStyle,code.FontWeight,code.FontStretch),code.FontSize,Brushes.Black,1.0);
        if(code.FontSize<32||text.Width+8>code.ActualWidth)throw new Exception("Lobby code unreadable/clipped "+UiText.Current);
        foreach(string name in new[]{"HostButton","JoinButton"})if(Get<Button>(name).Clip==null)throw new Exception("Missing rooftop sign outline");
        SetAccessibility(true);window.UpdateLayout();
        foreach(string name in new[]{"HostLabel","JoinLabel","GameStatus","SteamStatus","SettingsTitle","JoinCardTitle"}){
            var label=Get<TextBlock>(name);if(label.FontFamily.Source!="Segoe UI"||label.FontStyle!=FontStyles.Normal)throw new Exception("Accessibility font not applied "+name);
        }
        foreach(string name in new[]{"HostLabel","JoinLabel","GameStatus","SteamStatus"}){
            var label=Get<TextBlock>(name);if(label.Visibility!=Visibility.Visible)throw new Exception("Accessible live label is hidden: "+name);
            if(String.IsNullOrWhiteSpace(label.Text))throw new Exception("Accessible live label has no text: "+name);
            if(name=="HostLabel"||name=="JoinLabel"){
                var measured=new FormattedText(label.Text,System.Globalization.CultureInfo.InvariantCulture,FlowDirection.LeftToRight,new Typeface(label.FontFamily,label.FontStyle,label.FontWeight,label.FontStretch),label.FontSize,Brushes.Black,1.0);
                if(measured.Width>label.ActualWidth+2||measured.Height>label.ActualHeight+1)throw new Exception("Accessible action label does not fit its rooftop target: "+name+" "+UiText.Current);
            }
        }
        foreach(string name in new[]{"RoofCity","RoofTitle"})if(Get<Image>(name).Visibility!=Visibility.Collapsed)throw new Exception("Decorative rooftop layer remains visible in accessible mode: "+name);
        if(Math.Abs(Get<Image>("RoofFront").Opacity-0.42)>0.001)throw new Exception("Accessible rooftop contrast was not reduced");
        SetAccessibility(false);window.UpdateLayout();
        foreach(string name in new[]{"RoofCity","RoofTitle","RoofFront"})if(Get<Image>(name).Visibility!=Visibility.Visible)throw new Exception("Rooftop scene was not restored: "+name);
        if(Math.Abs(Get<Image>("RoofFront").Opacity-1.0)>0.001)throw new Exception("Rooftop foreground opacity was not restored");
        previewSession=false;Refresh();window.UpdateLayout();
    }

    private void CheckReviewMatrix(){
        OpenCard("SettingsCard");
        Get<TabControl>("SettingsTabs").SelectedIndex = 2;
        window.UpdateLayout();
        var heading = Get<TextBlock>("GuideHostTitle");
        var copy = Get<TextBlock>("GuideHostText");
        var headingSize = heading.FontSize;
        var copySize = copy.FontSize;
        var originalCopySizeSource = copy.ReadLocalValue(TextBlock.FontSizeProperty);
        for (int cycle = 0; cycle < 2; ++cycle) {
            SetAccessibility(true);
            window.UpdateLayout();
            SetAccessibility(false);
            window.UpdateLayout();
            if (heading.FontSize != headingSize || copy.FontSize != copySize ||
                !Object.Equals(originalCopySizeSource, copy.ReadLocalValue(TextBlock.FontSizeProperty)))
                throw new Exception("Settings font restoration: heading " + headingSize + " -> " + heading.FontSize +
                    ", body " + copySize + " -> " + copy.FontSize + "; style inheritance changed.");
        }
        CloseCard();
        foreach(var language in UiText.Languages){
            SetLanguage(language[0]);
            foreach(string state in new[]{"ready","missing","hosting","joining","error"}){
                ConfigurePreview(state);window.UpdateLayout();
                bool session=state=="hosting"||state=="joining"||state=="error";
                if((Get<Border>("SessionBar").Visibility==Visibility.Visible)!=session)throw new Exception("Incorrect session state "+state);
                SetAccessibility(true);window.UpdateLayout();
                if(Get<TextBlock>("HostLabel").Visibility!=Visibility.Visible||Get<TextBlock>("JoinLabel").Visibility!=Visibility.Visible||Get<TextBlock>("GameStatus").Visibility!=Visibility.Visible||Get<TextBlock>("SteamStatus").Visibility!=Visibility.Visible)
                    throw new Exception("Accessible rooftop labels/status hidden in "+state+" "+UiText.Current);
                if(Get<TextBlock>("GameStatus").Text!=UiText.Get(state=="missing"?"comicGameSetup":"comicGameReady")||Get<TextBlock>("SteamStatus").Text!="Steam: "+UiText.Get("comicReady"))
                    throw new Exception("Accessible status does not match backend state "+state+" "+UiText.Current);
                SetAccessibility(false);window.UpdateLayout();
                if(state=="hosting"&&Get<TextBox>("LobbyText").Text!=UInt64.MaxValue.ToString())throw new Exception("Hosting lobby missing");
                if(state=="joining"&&(Get<TextBox>("LobbyText").Visibility!=Visibility.Collapsed||Get<Button>("CopyLobby").Visibility!=Visibility.Collapsed))throw new Exception("Joining has an invented lobby");
                if(state=="error"&&Get<Grid>("LobbyRow").Visibility!=Visibility.Collapsed)throw new Exception("Error has inactive session controls");
                if(session)foreach(string name in new[]{"HostPaper","JoinPaper"}){var card=Get<Grid>(name);if(Canvas.GetTop(card)+card.Height>=Canvas.GetTop(Get<Border>("SessionBar")))throw new Exception("Session overlaps action labels");}
            }
            foreach(string state in new[]{"settings","join","accessible-settings","accessible-join"}){
                ConfigurePreview(state);window.UpdateLayout();
                string cardName=state.EndsWith("settings",StringComparison.Ordinal)?"SettingsCard":"JoinCard";
                var card=Get<Border>(cardName);
                foreach(string name in cardName=="JoinCard"?new[]{"JoinPaste","JoinCancel","JoinSubmit","JoinCardTitle"}:new[]{"DisplayTab","SetupTab","GuideTab","SettingsTitle"}){
                    var item=Get<FrameworkElement>(name);Rect bounds=item.TransformToAncestor(card).TransformBounds(new Rect(0,0,item.ActualWidth,item.ActualHeight));
                    if(bounds.Left<0||bounds.Right>card.ActualWidth+1)throw new Exception("Translated card control overflow "+name+" "+UiText.Current+" "+state);
                }
                if (cardName == "SettingsCard") {
                    string[] fields = {"DisplayBox","ImportButton","LogBox"};
                    var tabs = Get<TabControl>("SettingsTabs");
                    for (int i = 0; i < fields.Length; ++i) {
                        tabs.SelectedIndex = i;
                        window.UpdateLayout();
                        var field = Get<Control>(fields[i]);
                        if (!field.IsVisible || (field.ToolTip != null) != (i < 2))
                            throw new Exception("Settings tab content/hint failed: " + fields[i] + " " + UiText.Current);
                    }
                }
                CloseCard();
            }
        }
        SetLanguage("en");ConfigurePreview("hosting");
        Get<TextBlock>("ActivityText").Text=String.Join("\n",Enumerable.Repeat(UiText.Get("partnerWait"),12));window.UpdateLayout();
        var scroll=Get<ScrollViewer>("ActivityScroll");scroll.ScrollToEnd();window.UpdateLayout();
        if(scroll.ScrollableHeight<=0||scroll.VerticalOffset<=0)throw new Exception("Long status cannot be read by scrolling");scroll.ScrollToHome();
        ConfigurePreview("ready");
        Console.WriteLine("WPF_REVIEW_MATRIX_PASS 11-locales/5-states/normal-readable-dialog-fit/long-status-scroll/no-invented-join-code/no-overlap");
    }
}
}
