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
        internal DependencyObject Target;internal FontFamily Family;internal double Size;internal FontStyle Style;internal FontWeight Weight;
        internal FontState(TextBlock t){Target=t;Family=t.FontFamily;Size=t.FontSize;Style=t.FontStyle;Weight=t.FontWeight;}
        internal FontState(Control c){Target=c;Family=c.FontFamily;Size=c.FontSize;Style=c.FontStyle;Weight=c.FontWeight;}
        internal void Set(bool enabled){
            FontFamily family=enabled?new FontFamily("Segoe UI"):Family;double size=enabled?Math.Max(20,Size):Size;FontStyle style=enabled?FontStyles.Normal:Style;FontWeight weight=enabled?FontWeights.SemiBold:Weight;
            var t=Target as TextBlock;if(t!=null){t.FontFamily=family;t.FontSize=size;t.FontStyle=style;t.FontWeight=weight;return;}
            var c=(Control)Target;c.FontFamily=family;c.FontSize=size;c.FontStyle=style;c.FontWeight=weight;
        }
    }
    private static IEnumerable<DependencyObject> LogicalElements(DependencyObject root){yield return root;foreach(object item in LogicalTreeHelper.GetChildren(root)){var child=item as DependencyObject;if(child!=null)foreach(var element in LogicalElements(child))yield return element;}}
    private void InitializeAccessibility(){
        foreach(var element in LogicalElements(window)){
            var text=element as TextBlock;if(text!=null){normalFonts.Add(new FontState(text));continue;}
            var control=element as Control;if(control!=null)normalFonts.Add(new FontState(control));
        }
        var toggle=Get<ToggleButton>("AccessibilityButton");toggle.Checked+=delegate{SetAccessibility(true);};toggle.Unchecked+=delegate{SetAccessibility(false);};
    }
    private void SetAccessibility(bool value){
        if(accessible==value)return;accessible=value;
        foreach(var font in normalFonts)font.Set(value);
        foreach(string name in new[]{"HeaderArt","HostArt","JoinArt","TitleArt"})Get<Image>(name).Visibility=value?Visibility.Collapsed:Visibility.Visible;
        Get<TextBlock>("AccessibleTitle").Visibility=Get<TextBlock>("AccessibleSubtitle").Visibility=value?Visibility.Visible:Visibility.Collapsed;
        Get<Canvas>("Stage").Background=value?Brush("#ECEEF1"):Brushes.White;
        foreach(string prefix in new[]{"Host","Join"}){
            Get<Polygon>(prefix+"Ticket").Visibility=value?Visibility.Collapsed:Visibility.Visible;
            Get<Canvas>(prefix+"TicketText").RenderTransform=new RotateTransform(value?0:prefix=="Host"?-2.2:-1.9);
        }
        var tag=Get<TextBlock>("Tagline");tag.Foreground=value?Brushes.Black:Brushes.White;
        var tagBox=(FrameworkElement)tag.Parent;tagBox.RenderTransform=new RotateTransform(value?0:-2);
        SetActionLabel("HostLabel","comicHost");SetActionLabel("JoinLabel","comicJoin");UpdateLanguagePresentation();
        Get<ToggleButton>("AccessibilityButton").IsChecked=value;
    }
    private void LayoutSession(bool active){
        double height=active?328:510;
        foreach(string name in new[]{"HostButton","JoinButton"})if(Get<Button>(name).Height!=height)Get<Button>(name).Height=height;
    }
    private void UpdateLanguagePresentation(){
        var combo=Get<ComboBox>("LanguageBox");combo.ApplyTemplate();
        var traced=combo.Template.FindName("ReferenceEnglishText",combo) as FrameworkElement;
        var live=combo.Template.FindName("LanguageSelectionText",combo) as FrameworkElement;
        if(traced!=null)traced.Visibility=!accessible&&UiText.Current=="en"?Visibility.Visible:Visibility.Collapsed;
        if(live!=null)live.Visibility=accessible||UiText.Current!="en"?Visibility.Visible:Visibility.Collapsed;
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
        if(flags.Contains("settings"))OpenCard("SettingsCard");
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
        foreach(string name in new[]{"HostButton","JoinButton"})if(Get<Button>(name).Clip!=null)throw new Exception("Card is not rectangular");
        SetAccessibility(true);window.UpdateLayout();
        foreach(string name in new[]{"HostLabel","JoinLabel","Tagline","SettingsTitle","HelpCardTitle","JoinCardTitle"}){
            var label=Get<TextBlock>(name);if(label.FontFamily.Source!="Segoe UI"||label.FontStyle!=FontStyles.Normal)throw new Exception("Accessibility font not applied "+name);
        }
        foreach(string prefix in new[]{"Host","Join"})if(((RotateTransform)Get<Canvas>(prefix+"TicketText").RenderTransform).Angle!=0||Get<Polygon>(prefix+"Ticket").Visibility!=Visibility.Collapsed)throw new Exception("Accessibility board not straight");
        foreach(string name in new[]{"HeaderArt","HostArt","JoinArt","TitleArt"})if(Get<Image>(name).Visibility!=Visibility.Collapsed)throw new Exception("Accessibility art was not hidden");
        SetAccessibility(false);window.UpdateLayout();
        if(Get<TextBlock>("HostLabel").FontFamily.Source!="Impact"&&UiText.Current!="zh")throw new Exception("Comic font not restored");
        if(Get<Image>("HeaderArt").Visibility!=Visibility.Visible)throw new Exception("Comic artwork not restored");
        previewSession=false;Refresh();window.UpdateLayout();
    }

    private void CheckReviewMatrix(){
        foreach(var language in UiText.Languages){
            SetLanguage(language[0]);
            foreach(string state in new[]{"ready","missing","hosting","joining","error"}){
                ConfigurePreview(state);window.UpdateLayout();
                bool session=state=="hosting"||state=="joining"||state=="error";
                if((Get<Border>("SessionBar").Visibility==Visibility.Visible)!=session)throw new Exception("Incorrect session state "+state);
                if(state=="hosting"&&Get<TextBox>("LobbyText").Text!=UInt64.MaxValue.ToString())throw new Exception("Hosting lobby missing");
                if(state=="joining"&&(Get<TextBox>("LobbyText").Visibility!=Visibility.Collapsed||Get<Button>("CopyLobby").Visibility!=Visibility.Collapsed))throw new Exception("Joining has an invented lobby");
                if(state=="error"&&Get<Grid>("LobbyRow").Visibility!=Visibility.Collapsed)throw new Exception("Error has inactive session controls");
                if(session)foreach(string name in new[]{"HostButton","JoinButton"}){var card=Get<Button>(name);if(Canvas.GetTop(card)+card.Height>=Canvas.GetTop(Get<Border>("SessionBar")))throw new Exception("Session overlaps action labels");}
            }
            foreach(string state in new[]{"settings","join","accessible-settings","accessible-join"}){
                ConfigurePreview(state);window.UpdateLayout();
                string cardName=state.EndsWith("settings",StringComparison.Ordinal)?"SettingsCard":"JoinCard";
                var card=Get<Border>(cardName);
                foreach(string name in cardName=="JoinCard"?new[]{"JoinPaste","JoinCancel","JoinSubmit","JoinCardTitle"}:new[]{"VerifyButton","ImportButton","HelpButton","SettingsTitle"}){
                    var item=Get<FrameworkElement>(name);Rect bounds=item.TransformToAncestor(card).TransformBounds(new Rect(0,0,item.ActualWidth,item.ActualHeight));
                    if(bounds.Left<0||bounds.Right>card.ActualWidth+1)throw new Exception("Translated card control overflow "+name+" "+UiText.Current+" "+state);
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
