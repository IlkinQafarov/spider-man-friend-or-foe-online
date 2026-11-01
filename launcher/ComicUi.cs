using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace FofPrivate {
static class ComicArt {
    public static readonly Color Paper=Color.FromArgb(255,248,231),Ink=Color.FromArgb(12,13,17);
    public static Image Load(string name){return LoadFile("backgrounds",name);}
    public static Image LoadFile(string layer,string name){
        string path=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"assets",layer,name+".png");
        try{Files.NoReparse(path);using(var source=Image.FromFile(path))return new Bitmap(source);}catch(Exception){return null;}
    }
    public static void Cover(Graphics g,Image image,Rectangle bounds,Color fallback){
        if(bounds.Width<1||bounds.Height<1)return;
        if(image==null){using(var b=new SolidBrush(fallback))g.FillRectangle(b,bounds);return;}
        float scale=Math.Max((float)bounds.Width/image.Width,(float)bounds.Height/image.Height);
        float w=image.Width*scale,h=image.Height*scale;g.DrawImage(image,bounds.X+(bounds.Width-w)/2,bounds.Y+(bounds.Height-h)/2,w,h);
    }
    public static void Letter(Graphics g,string text,Rectangle box,float unused,Color fill){
        if(box.Width<2||box.Height<2)return;
        string family=UiText.Current=="zh"?"Microsoft YaHei UI":"Impact";
        using(var path=new GraphicsPath())using(var ff=new FontFamily(family)){
            path.AddString(text,ff,(int)FontStyle.Regular,100,Point.Empty,StringFormat.GenericTypographic);
            using(var slant=new Matrix(1,0,-.13f,1,0,0))path.Transform(slant);
            RectangleF b=path.GetBounds();if(b.Width<1||b.Height<1)return;
            float scale=Math.Min(box.Width/b.Width,box.Height/b.Height);
            using(var matrix=new Matrix(scale,0,0,scale,box.X+(box.Width-b.Width*scale)/2-b.X*scale,box.Y+(box.Height-b.Height*scale)/2-b.Y*scale))path.Transform(matrix);
            using(var brush=new SolidBrush(fill))g.FillPath(brush,path);
        }
    }
    public static void Person(Graphics g,float x,float y,float s){
        using(var p=new GraphicsPath()){
            p.AddBezier(x-s,y+s*.85f,x-s,y+s*.1f,x+s,y+s*.1f,x+s,y+s*.85f);p.CloseFigure();
            using(var b=new SolidBrush(Paper))g.FillPath(b,p);using(var pen=new Pen(Ink,s*.12f)){pen.LineJoin=LineJoin.Round;g.DrawPath(pen,p);}
        }
        using(var b=new SolidBrush(Paper))g.FillEllipse(b,x-s*.48f,y-s*.85f,s*.96f,s*.96f);
        using(var pen=new Pen(Ink,s*.12f))g.DrawEllipse(pen,x-s*.48f,y-s*.85f,s*.96f,s*.96f);
    }
    public static GraphicsPath Capsule(float x,float y,float w,float h){var p=new GraphicsPath();p.AddArc(x,y,h,h,90,180);p.AddArc(x+w-h,y,h,h,270,180);p.CloseFigure();return p;}
}
sealed class ComicAction : Button {
    readonly Image art;readonly bool hosting;bool hover;
    public string Subtitle="";
    public ComicAction(bool isHost){hosting=isHost;art=ComicArt.Load(hosting?"host":"join");AutoSize=false;Cursor=Cursors.Hand;DoubleBuffered=true;SetStyle(ControlStyles.ResizeRedraw,true);}
    PointF[] Shape(){return hosting?new[]{new PointF(0,0),new PointF(Width*.95f,Height*.14f),new PointF(Width,Height),new PointF(0,Height)}:new[]{new PointF(0,Height*.14f),new PointF(Width,0),new PointF(Width*.94f,Height),new PointF(Width*.055f,Height)};}
    protected override void OnResize(EventArgs e){base.OnResize(e);if(Width<1||Height<1)return;using(var p=new GraphicsPath()){p.AddPolygon(Shape());var old=Region;Region=new Region(p);if(old!=null)old.Dispose();}}
    protected override void OnMouseEnter(EventArgs e){hover=true;Invalidate();base.OnMouseEnter(e);}
    protected override void OnMouseLeave(EventArgs e){hover=false;Invalidate();base.OnMouseLeave(e);}
    protected override void OnPaint(PaintEventArgs e){
        var g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;g.InterpolationMode=InterpolationMode.HighQualityBicubic;var r=ClientRectangle;if(r.Width<2||r.Height<2)return;
        ComicArt.Cover(g,art,r,hosting?Theme.Red:Theme.Blue);float k=Width/800f;
        using(var edge=new Pen(hover&&Enabled?Color.White:ComicArt.Ink,8*k)){edge.LineJoin=LineJoin.Miter;g.DrawPolygon(edge,Shape());}
        float cx=Width*.52f,cy=Height*.40f,s=Height*.20f;
        if(hosting){ComicArt.Person(g,cx,cy,s);using(var brush=new SolidBrush(ComicArt.Paper))g.FillEllipse(brush,cx+s*.36f,cy+s*.04f,s*1.05f,s*1.05f);
            using(var pen=new Pen(ComicArt.Ink,s*.13f)){g.DrawEllipse(pen,cx+s*.36f,cy+s*.04f,s*1.05f,s*1.05f);g.DrawLine(pen,cx+s*.58f,cy+s*.57f,cx+s*1.18f,cy+s*.57f);g.DrawLine(pen,cx+s*.88f,cy+s*.27f,cx+s*.88f,cy+s*.87f);}}
        else {ComicArt.Person(g,cx-s*.73f,cy,s*.88f);ComicArt.Person(g,cx+s*.93f,cy,s*.88f);
            var state=g.Save();g.TranslateTransform(cx+s*.08f,cy+s*.65f);g.RotateTransform(-45);
            using(var a=ComicArt.Capsule(-s*.66f,-s*.29f,s*1.12f,s*.55f))using(var b=ComicArt.Capsule(-s*.1f,-s*.29f,s*1.12f,s*.55f))using(var fill=new SolidBrush(ComicArt.Paper))using(var edge=new Pen(ComicArt.Ink,s*.12f)){g.FillPath(fill,a);g.DrawPath(edge,a);g.FillPath(fill,b);g.DrawPath(edge,b);g.DrawLine(edge,-s*.16f,0,s*.43f,0);}g.Restore(state);}
        // Torn paper is foreground geometry. Text is a real translated button label.
        float top=Height*.62f;PointF[] paper={new PointF(Width*.14f,top+Height*.02f),new PointF(Width*.97f,top-Height*.025f),new PointF(Width*.94f,top+Height*.045f),new PointF(Width*.98f,top+Height*.075f),new PointF(Width*.94f,top+Height*.105f),new PointF(Width*.96f,Height*.91f),new PointF(Width*.12f,Height*.96f),new PointF(Width*.14f,Height*.92f),new PointF(Width*.1f,Height*.89f),new PointF(Width*.13f,top+Height*.12f),new PointF(Width*.1f,top+Height*.085f)};
        using(var b=new SolidBrush(ComicArt.Paper))g.FillPolygon(b,paper);using(var pen=new Pen(ComicArt.Ink,2*k))g.DrawPolygon(pen,paper);
        string upper=Text.ToUpper(UiText.Current=="tr"?new System.Globalization.CultureInfo("tr-TR"):System.Globalization.CultureInfo.InvariantCulture);
        ComicArt.Letter(g,upper,new Rectangle((int)(Width*.17f),(int)(Height*.665f),(int)(Width*.73f),(int)(Height*.16f)),0,ComicArt.Ink);
        using(var font=new Font("Segoe UI",Math.Max(9,Height*.039f),FontStyle.Bold))TextRenderer.DrawText(g,Subtitle,font,new Rectangle((int)(Width*.16f),(int)(Height*.84f),(int)(Width*.72f),(int)(Height*.085f)),ComicArt.Ink,TextFormatFlags.HorizontalCenter|TextFormatFlags.VerticalCenter|TextFormatFlags.NoPrefix);
        using(var pen=new Pen(hosting?Theme.Red:Theme.Blue,4*k)){float ax=Width*.88f,ay=Height*.885f;g.DrawLine(pen,ax-18*k,ay,ax+16*k,ay);g.DrawLine(pen,ax+16*k,ay,ax+3*k,ay-12*k);g.DrawLine(pen,ax+16*k,ay,ax+3*k,ay+12*k);}
        if(!Enabled)using(var dim=new SolidBrush(Color.FromArgb(130,ComicArt.Paper)))g.FillRectangle(dim,r);
        if(Focused)using(var p=new Pen(Color.White,2)){p.DashStyle=DashStyle.Dash;g.DrawPolygon(p,Shape());}
    }
    protected override void Dispose(bool disposing){if(disposing&&art!=null)art.Dispose();base.Dispose(disposing);}
}
sealed class ComicTool : Button {
    public ComicTool(){FlatStyle=FlatStyle.Flat;FlatAppearance.BorderColor=ComicArt.Ink;FlatAppearance.BorderSize=2;BackColor=ComicArt.Paper;ForeColor=ComicArt.Ink;Cursor=Cursors.Hand;Font=new Font("Segoe UI",12,FontStyle.Bold);}
}
sealed class ComicStatus : Control {
    public bool Ready;
    public ComicStatus(){DoubleBuffered=true;BackColor=ComicArt.Paper;}
    protected override void OnPaint(PaintEventArgs e){var g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;float d=Height*.82f;
        using(var b=new SolidBrush(Ready?Color.FromArgb(29,133,60):Color.FromArgb(180,119,28)))g.FillEllipse(b,0,(Height-d)/2,d,d);
        using(var pen=new Pen(Color.White,Math.Max(2,d*.075f))){if(Ready){g.DrawLines(pen,new[]{new PointF(d*.24f,Height*.51f),new PointF(d*.43f,Height*.68f),new PointF(d*.77f,Height*.32f)});}else{g.DrawLine(pen,d*.5f,Height*.28f,d*.5f,Height*.56f);g.DrawLine(pen,d*.5f,Height*.66f,d*.5f,Height*.73f);}}
        using(var f=new Font("Segoe UI",Math.Max(9,Height*.25f),FontStyle.Bold))TextRenderer.DrawText(g,Text,f,new Rectangle((int)d+12,0,Width-(int)d-12,Height),ComicArt.Ink,TextFormatFlags.Left|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix);
    }
}
sealed partial class LauncherForm {
    readonly Button setupToggle=new ComicTool(),detailsToggle=new ComicTool(),minimizeComic=new ComicTool(),maximizeComic=new ComicTool(),closeComic=new ComicTool();
    bool joinConfirmed,reviewOnly;Panel setupPanel;Label activity;Form settingsDialog,detailsDialog,codeDialog;Panel sessionBar;
    readonly ComicStatus steamStatus=new ComicStatus(),gameStatus=new ComicStatus();
    readonly Button languageComic=new ComicTool(),confirmJoin=new ComicTool();readonly ContextMenuStrip languageMenu=new ContextMenuStrip();
    readonly TextBox shareCode=new TextBox{ReadOnly=true};readonly Button shareCopy=new ComicTool();
    [DllImport("user32.dll")]static extern bool ReleaseCapture();
    [DllImport("user32.dll")]static extern IntPtr SendMessage(IntPtr hwnd,int message,IntPtr wparam,IntPtr lparam);
    void BuildComicLayout(){
        Font=new Font("Segoe UI",10);AutoScaleMode=AutoScaleMode.None;FormBorderStyle=FormBorderStyle.None;ClientSize=new Size(1152,768);MinimumSize=new Size(900,600);StartPosition=FormStartPosition.CenterScreen;BackColor=ComicArt.Paper;ForeColor=ComicArt.Ink;
        Controls.AddRange(new Control[]{banner,host,join,gameStatus,steamStatus,import,detailsToggle,setupToggle,minimizeComic,maximizeComic,closeComic});
        host.AutoSize=join.AutoSize=false;host.MinimumSize=join.MinimumSize=Size.Empty;
        banner.Controls.Add(uiLanguage);uiLanguage.Visible=false;banner.Controls.Add(languageComic);banner.Controls.Add(setupToggle);banner.Controls.Add(minimizeComic);banner.Controls.Add(maximizeComic);banner.Controls.Add(closeComic);
        languageComic.Click+=(s,e)=>languageMenu.Show(languageComic,new Point(0,languageComic.Height));foreach(var entry in UiText.Languages){string code=entry[0];var item=new ToolStripMenuItem(entry[1]);item.Click+=(s,e)=>{foreach(LanguageItem choice in uiLanguage.Items)if(choice.Code==code)uiLanguage.SelectedItem=choice;};languageMenu.Items.Add(item);}
        uiLanguage.Width=175;uiLanguage.FlatStyle=FlatStyle.Flat;uiLanguage.BackColor=ComicArt.Paper;uiLanguage.ForeColor=ComicArt.Ink;
        setupToggle.Text="⚙";minimizeComic.Text="−";maximizeComic.Text="□";closeComic.Text="×";
        minimizeComic.Click+=(s,e)=>WindowState=FormWindowState.Minimized;maximizeComic.Click+=(s,e)=>WindowState=WindowState==FormWindowState.Maximized?FormWindowState.Normal:FormWindowState.Maximized;closeComic.Click+=(s,e)=>Close();
        banner.MouseDown+=(s,e)=>{if(e.Button==MouseButtons.Left){ReleaseCapture();SendMessage(Handle,0xA1,new IntPtr(2),IntPtr.Zero);}};
        gameStatus.Font=steamStatus.Font=new Font("Segoe UI",12,FontStyle.Bold);gameStatus.BackColor=steamStatus.BackColor=ComicArt.Paper;
        settingsDialog=NewComicDialog("Settings",900,270);setupPanel=new Panel{Dock=DockStyle.Fill,AutoScroll=true,Padding=new Padding(18)};settingsDialog.Controls.Add(setupPanel);
        var grid=new TableLayoutPanel{Dock=DockStyle.Top,AutoSize=true,ColumnCount=3};grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        language.Items.AddRange(new object[]{new Choice("English","gameEnglish"),new Choice("Russian","gameRussian")});language.SelectedIndex=0;
        grid.Controls.Add(lblGame,0,0);grid.Controls.Add(language,1,0);grid.Controls.Add(verify,2,0);grid.Controls.Add(lblSteam,0,1);grid.Controls.Add(dll,1,1);dll.Dock=DockStyle.Fill;grid.Controls.Add(browse,2,1);
        var displayRow=new FlowLayoutPanel{AutoSize=true,WrapContents=true};displayRow.Controls.AddRange(new Control[]{displayMode,resolution});grid.Controls.Add(lblDisplay,0,2);grid.Controls.Add(displayRow,1,2);grid.SetColumnSpan(displayRow,2);
        status.MaximumSize=new Size(700,0);grid.Controls.Add(status,0,3);grid.SetColumnSpan(status,3);
#if !PLAYER
        grid.Controls.Add(automated,0,4);grid.SetColumnSpan(automated,3);
#endif
        setupPanel.Controls.Add(grid);Stylize(grid);displayMode.Width=400;displayMode.DropDownWidth=600;foreach(var b in new[]{verify,browse})b.FlatAppearance.BorderColor=ComicArt.Ink;
        detailsDialog=NewComicDialog("Help",820,490);log.Dock=DockStyle.Fill;detailsDialog.Controls.Add(log);
        activity=new Label{AutoSize=false,AutoEllipsis=true,BackColor=ComicArt.Paper,Padding=new Padding(8),TextAlign=ContentAlignment.MiddleLeft};
        codeDialog=NewComicDialog("Join a friend",600,210);var codes=new TableLayoutPanel{Dock=DockStyle.Top,AutoSize=true,Padding=new Padding(20),ColumnCount=3,RowCount=3};
        codes.RowStyles.Add(new RowStyle(SizeType.AutoSize));codes.RowStyles.Add(new RowStyle(SizeType.AutoSize));codes.RowStyles.Add(new RowStyle(SizeType.Absolute,52));codes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));codes.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));codes.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        codes.Controls.Add(lblLobby,0,0);codes.SetColumnSpan(lblLobby,3);lobby.Dock=DockStyle.Fill;lobby.MaxLength=20;codes.Controls.Add(lobby,0,1);codes.Controls.Add(copy,1,1);codes.Controls.Add(paste,2,1);
        var confirm=confirmJoin;confirm.Dock=DockStyle.Fill;confirm.Height=45;codes.Controls.Add(confirm,0,2);codes.SetColumnSpan(confirm,3);codeDialog.Controls.Add(codes);codeDialog.AcceptButton=confirm;confirm.Enabled=false;lobby.TextChanged+=(s,e)=>{ulong value;confirm.Enabled=UInt64.TryParse(lobby.Text,System.Globalization.NumberStyles.None,System.Globalization.CultureInfo.InvariantCulture,out value)&&value>0;};confirm.Click+=(s,e)=>{if(!confirm.Enabled)return;joinConfirmed=true;codeDialog.DialogResult=DialogResult.OK;codeDialog.Close();};
        sessionBar=new Panel{Visible=false,BackColor=ComicArt.Paper};sessionBar.Controls.Add(activity);sessionBar.Controls.Add(stop);sessionBar.Controls.Add(shareCode);sessionBar.Controls.Add(shareCopy);shareCopy.Click+=(s,e)=>{if(shareCode.Text.Length>0)Clipboard.SetText(shareCode.Text);};lobby.TextChanged+=(s,e)=>{shareCode.Text=lobby.Text;UpdateComicSession();};Controls.Add(sessionBar);
        setupToggle.Click+=(s,e)=>settingsDialog.ShowDialog(this);detailsToggle.Click+=(s,e)=>detailsDialog.ShowDialog(this);
        dll.TextChanged+=(s,e)=>RefreshComicSteam();status.TextChanged+=(s,e)=>RefreshComicGame();
        Theme.Outline(copy);Theme.Outline(paste);Theme.Outline(stop);Theme.Outline(import);Theme.Outline(detailsToggle);
        foreach(var b in new[]{copy,paste,stop,import,detailsToggle}){b.FlatAppearance.BorderColor=ComicArt.Ink;b.ForeColor=ComicArt.Ink;b.BackColor=ComicArt.Paper;}
        import.FlatAppearance.BorderSize=detailsToggle.FlatAppearance.BorderSize=0;import.ForeColor=Theme.Blue;import.Font=new Font("Segoe UI",11,FontStyle.Underline);import.Dock=DockStyle.None;host.AutoSize=join.AutoSize=false;host.MinimumSize=join.MinimumSize=Size.Empty;
        Resize+=(s,e)=>SizeComicContent();FormClosed+=(s,e)=>{settingsDialog.Dispose();detailsDialog.Dispose();codeDialog.Dispose();languageMenu.Dispose();};
        SizeComicContent();
    }
    Form NewComicDialog(string title,int w,int h){return new Form{Text=title,ClientSize=new Size(w,h),MinimumSize=new Size(550,200),StartPosition=FormStartPosition.CenterParent,BackColor=ComicArt.Paper,ForeColor=ComicArt.Ink,Font=new Font("Segoe UI",11),ShowInTaskbar=false,MinimizeBox=false,MaximizeBox=false};}
    public void EnableReview(){reviewOnly=true;}
    bool AskJoinCode(){if(offline&&!reviewOnly)return true;joinConfirmed=false;codeDialog.DialogResult=DialogResult.None;codeDialog.Text=UiText.Get("comicJoin");codeDialog.ShowDialog(this);return joinConfirmed;}
    void Place(Control c,float x,float y,float w,float h){c.Bounds=new Rectangle((int)(x*ClientSize.Width/1536f),(int)(y*ClientSize.Height/1024f),(int)(w*ClientSize.Width/1536f),(int)(h*ClientSize.Height/1024f));}
    void SizeComicContent(){if(banner==null)return;banner.Dock=DockStyle.None;Place(banner,0,0,1536,466);Place(host,0,382,808,503);Place(join,785,397,751,488);
        float sx=ClientSize.Width/1536f,sy=ClientSize.Height/1024f;languageComic.Bounds=new Rectangle((int)(1060*sx),(int)(22*sy),(int)(185*sx),(int)(48*sy));
        setupToggle.Bounds=new Rectangle((int)(1260*sx),(int)(22*sy),(int)(62*sx),(int)(52*sy));minimizeComic.Bounds=new Rectangle((int)(1335*sx),(int)(22*sy),(int)(56*sx),(int)(52*sy));maximizeComic.Bounds=new Rectangle((int)(1390*sx),(int)(22*sy),(int)(56*sx),(int)(52*sy));closeComic.Bounds=new Rectangle((int)(1445*sx),(int)(22*sy),(int)(66*sx),(int)(52*sy));
        Place(gameStatus,72,928,285,47);Place(steamStatus,395,928,380,47);Place(import,880,925,400,52);Place(detailsToggle,1330,925,175,52);
        Place(sessionBar,25,897,1486,110);activity.Bounds=new Rectangle(8,4,Math.Max(100,sessionBar.Width-175),sessionBar.Height/2-4);shareCode.Bounds=new Rectangle(8,sessionBar.Height/2,Math.Max(100,sessionBar.Width-360),28);shareCopy.Bounds=new Rectangle(Math.Max(110,sessionBar.Width-340),sessionBar.Height/2-3,160,36);stop.Bounds=new Rectangle(Math.Max(0,sessionBar.Width-160),14,150,48);
        host.BringToFront();join.BringToFront();sessionBar.BringToFront();Invalidate();}
    void UpdateComicSession(){if(sessionBar==null)return;sessionBar.Visible=busy||relay!=null||launcher!=null||(activity!=null&&activity.Text.StartsWith("REJECTED:",StringComparison.Ordinal));shareCode.Visible=shareCopy.Visible=lobby.Text.Length>0;sessionBar.BringToFront();}
    void RefreshComicGame(){if(gameStatus==null)return;bool found=Directory.Exists(TestDirectory());gameStatus.Ready=found;gameStatus.Text=UiText.Get(found?"comicGameReady":"comicGameSetup");gameStatus.Invalidate();gameStatus.ForeColor=ComicArt.Ink;}
    void RefreshComicSteam(){steamStatus.Ready=dll.Text.Length>0;steamStatus.Text="Steam: "+UiText.Get(dll.Text.Length>0?"comicReady":"comicSetup");steamStatus.Invalidate();}
    void ComicTexts(){confirmJoin.Text=UiText.Get("comicJoin")+" →";languageComic.Text="◎  "+(uiLanguage.SelectedItem==null?"English":uiLanguage.SelectedItem.ToString())+"  ⌄";shareCopy.Text=UiText.Get("copy");detailsToggle.Text="ⓘ  "+UiText.Get("comicHelp");import.Text="▱  "+UiText.Get("comicChangeFolder");settingsDialog.Text=UiText.Get("comicSettings");detailsDialog.Text=UiText.Get("comicHelp");
        host.Text=UiText.Get("comicHost");join.Text=UiText.Get("comicJoin");((ComicAction)host).Subtitle=UiText.Get("comicHostSub");((ComicAction)join).Subtitle=UiText.Get("comicJoinSub");RefreshComicSteam();RefreshComicGame();host.Invalidate();join.Invalidate();SizeComicContent();}
    protected override void OnPaint(PaintEventArgs e){base.OnPaint(e);var g=e.Graphics;g.SmoothingMode=SmoothingMode.AntiAlias;
        using(var blue=new SolidBrush(Theme.Blue))g.FillRectangle(blue,0,ClientSize.Height*.97f,ClientSize.Width,ClientSize.Height*.03f);
        using(var pen=new Pen(ComicArt.Ink,4)){g.DrawLine(pen,0,ClientSize.Height*.88f,ClientSize.Width,ClientSize.Height*.88f);}
        float k=ClientSize.Width/1536f;using(var brush=new SolidBrush(Theme.Blue))for(int x=0;x<50;x+=7)for(int y=900;y<1010;y+=8)g.FillEllipse(brush,x*k,y*ClientSize.Height/1024f,2*k,2*k);
        using(var pen=new Pen(Color.FromArgb(120,80,70,50),1)){g.DrawLine(pen,ClientSize.Width*.22f,ClientSize.Height*.91f,ClientSize.Width*.22f,ClientSize.Height*.96f);g.DrawLine(pen,ClientSize.Width*.57f,ClientSize.Height*.91f,ClientSize.Width*.57f,ClientSize.Height*.96f);}
    }
    protected override void WndProc(ref Message m){base.WndProc(ref m);if(m.Msg==0x84&&(int)m.Result==1&&WindowState==FormWindowState.Normal){Point p=PointToClient(Cursor.Position);int edge=6;bool l=p.X<edge,r=p.X>ClientSize.Width-edge,t=p.Y<edge,b=p.Y>ClientSize.Height-edge;m.Result=new IntPtr(t?(l?13:r?14:12):b?(l?16:r?17:15):l?10:r?11:1);}}
    public void RenderPreview(string path){using(var bitmap=new Bitmap(Width,Height)){DrawToBitmap(bitmap,new Rectangle(0,0,Width,Height));bitmap.Save(path,System.Drawing.Imaging.ImageFormat.Png);}}
    public void PreviewLanguage(string code,bool settings){foreach(LanguageItem item in uiLanguage.Items)if(item.Code==code)uiLanguage.SelectedItem=item;if(settings)settingsDialog.Show(this);PerformLayout();}
    public void CheckComicLayout(){if(host.Width<250||host.Height<200||join.Width<250||!detailsToggle.Visible)throw new Exception("Comic actions/footer layout failed");if(!host.CanSelect||!join.CanSelect)throw new Exception("Comic keyboard access failed");foreach(string name in new[]{"header","host","join"})using(var art=ComicArt.Load(name))if(art==null)throw new Exception("Missing background");using(var logo=ComicArt.LoadFile("foreground","title"))if(logo==null)throw new Exception("Missing title foreground");}
}
}
