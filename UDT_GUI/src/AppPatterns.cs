using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;


namespace Uber.DemoTools
{
    public class PatternsComponent : AppComponent
    {
        public FrameworkElement RootControl { get; private set; }
        public List<DemoInfoListView> AllListViews { get { return GetAllListViews(); } }
        public List<DemoInfoListView> InfoListViews { get { return GetInfoListViews(); } }
        public ComponentType Type { get { return ComponentType.Patterns; } }

        public PatternsComponent(App app)
        {
            _app = app;
            RootControl = CreateTab();
        }

        public void PopulateViews(DemoInfo demoInfo)
        {
            foreach(var component in _components)
            {
                component.PopulateViews(demoInfo);
            }
        }

        public void SaveToConfigObject(UdtConfig config)
        {
            foreach(var component in _components)
            {
                component.SaveToConfigObject(config);
            }
        }

        public void SaveToConfigObject(UdtPrivateConfig config)
        {
            foreach(var component in _components)
            {
                component.SaveToConfigObject(config);
            }
        }

        private App _app;
        private List<AppComponent> _components = new List<AppComponent>();

        private FrameworkElement CreateTab()
        {
            var chat = new ChatFiltersComponent(_app);
            _components.Add(chat);
            var chatTab = new TabItem();
            chatTab.Header = "Global Chat";
            chatTab.Content = chat.RootControl;

            var frag = new FragSequenceFiltersComponent(_app);
            _components.Add(frag);
            var fragTab = new TabItem();
            fragTab.Header = "Frag Sequence";
            fragTab.Content = frag.RootControl;

            var midAir = new MidAirFiltersComponent(_app);
            _components.Add(midAir);
            var midAirTab = new TabItem();
            midAirTab.Header = "Mid-Air Frags";
            midAirTab.Content = midAir.RootControl;

            var tabControl = new TabControl();
            tabControl.HorizontalAlignment = HorizontalAlignment.Stretch;
            tabControl.VerticalAlignment = VerticalAlignment.Stretch;
            tabControl.Margin = new Thickness(5);
            tabControl.Items.Add(chatTab);
            tabControl.Items.Add(fragTab);
            tabControl.Items.Add(midAirTab);

            return tabControl;
        }

        private List<DemoInfoListView> GetAllListViews()
        {
            var list = new List<DemoInfoListView>();
            foreach(var component in _components)
            {
                var views = component.AllListViews;
                if(views != null)
                {
                    list.AddRange(views);
                }
            }

            return list;
        }

        private List<DemoInfoListView> GetInfoListViews()
        {
            var list = new List<DemoInfoListView>();
            foreach(var component in _components)
            {
                var views = component.InfoListViews;
                if(views != null)
                {
                    list.AddRange(views);
                }
            }

            return list;
        }
    }
}
