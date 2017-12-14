#include <gtest/gtest.h>

#include "simppl/stub.h"
#include "simppl/skeleton.h"
#include "simppl/dispatcher.h"
#include "simppl/interface.h"

#include <thread>


using namespace std::literals::chrono_literals;

using simppl::dbus::in;
using simppl::dbus::out;


namespace test
{

   
struct Complex
{
   typedef typename simppl::dbus::make_serializer<std::string, std::string>::type serializer_type;
   
   std::string str1;
   std::string str2;
   
   Complex() = default;
   
   Complex(const Complex& c)
   {
      str1 = c.str1;
      str2 = c.str2;
      
      ++rvo_count;
   }
   
   Complex& operator=(const Complex& c)
   {
      str1 = c.str1;
      str2 = c.str2;
      
      ++rvo_count;
      return *this;
   }
   
   // not serialized
   static int rvo_count;
};


/*static*/ int Complex::rvo_count;


INTERFACE(Simple)
{
   Method<> hello;
   Method<> hello_wait_for_some_time;

   Method<in<int>, simppl::dbus::oneway> oneway;

   Method<in<int>, in<double>, out<double>> add;
   Method<in<int>, in<double>, out<int>, out<double>> echo;

   Method<in<std::wstring>, out<std::wstring>> echo_wstring;
   Method<in<wchar_t*>, out<wchar_t*>>         echo_wchart;

   Method<out<Complex>>                        test_rvo;

   Property<int> data;

   Signal<int> sig;
   Signal<int, int> sig2;

   inline
   Simple()
    : INIT(hello)
    , INIT(hello_wait_for_some_time)
    , INIT(oneway)
    , INIT(add)
    , INIT(echo)
    , INIT(echo_wstring)
    , INIT(echo_wchart)
    , INIT(test_rvo)
    , INIT(data)
    , INIT(sig)
    , INIT(sig2)
   {
      // NOOP
   }
};

}

using namespace test;


namespace {


struct Client : simppl::dbus::Stub<Simple>
{
   Client(simppl::dbus::Dispatcher& d)
    : simppl::dbus::Stub<Simple>(d, "s")
   {
      connected >> [this](simppl::dbus::ConnectionState s){
         EXPECT_EQ(simppl::dbus::ConnectionState::Connected, s);

         this->hello.async() >> [this](simppl::dbus::CallState state){
            EXPECT_TRUE((bool)state);

            this->oneway(42);

            const wchar_t* wct = L"Hello world";

            this->echo_wchart.async(wct) >> [this](simppl::dbus::CallState state, wchar_t* p){
               EXPECT_TRUE((bool)state);
               EXPECT_EQ(0, wcscmp(p, L"Hello world"));

               // must delete pointer now, it's mine
               simppl::dbus::detail::Deserializer::free(p);

               // shutdown
               this->oneway(7777);
            };
         };
      };
   }
};


struct CancelClient : simppl::dbus::Stub<Simple>
{
   CancelClient(simppl::dbus::Dispatcher& d)
    : simppl::dbus::Stub<Simple>(d, "s")
   {
      connected >> [this](simppl::dbus::ConnectionState s){

         if (s == simppl::dbus::ConnectionState::Connected)   // FIXME add bool cast operator
         {
            simppl::dbus::PendingCall p = this->hello_wait_for_some_time.async() >> [this](simppl::dbus::CallState state){

                // never called!!!
                EXPECT_TRUE(false);
                if (!state)
                    std::cout << state.what() << std::endl;
            };

            std::this_thread::sleep_for(100ms);
            
            p.cancel();
            
            // must stop server
            oneway(7777);
         }
         else
            disp().stop();
      };
   }
};


struct DisconnectClient : simppl::dbus::Stub<Simple>
{
   DisconnectClient(simppl::dbus::Dispatcher& d)
    : simppl::dbus::Stub<Simple>(d, "s")
   {
      connected >> [this](simppl::dbus::ConnectionState s)
      {
         EXPECT_EQ(this->expected_, s);

         if (s == simppl::dbus::ConnectionState::Connected)
         {
            this->oneway(7777);
         }
         else
            this->disp().stop();

         this->expected_ = simppl::dbus::ConnectionState::Disconnected;
      };
   }


   simppl::dbus::ConnectionState expected_ = simppl::dbus::ConnectionState::Connected;
};


struct PropertyClient : simppl::dbus::Stub<Simple>
{
   PropertyClient(simppl::dbus::Dispatcher& d)
    : simppl::dbus::Stub<Simple>(d, "sa")
   {
      connected >> [this](simppl::dbus::ConnectionState s)
      {
         EXPECT_EQ(simppl::dbus::ConnectionState::Connected, s);

         // like for signals, attributes must be attached when the client is connected
         this->data.attach() >> [this](simppl::dbus::CallState state, int new_value)
         {
            this->attributeChanged(state, new_value);
         };
      };
   }

   void attributeChanged(simppl::dbus::CallState state, int new_value)
   {
      EXPECT_TRUE((bool)state);

      if (first_)
      {
         // first you get the current value
         EXPECT_EQ(4711, new_value);

         // now we trigger update on server
         oneway(42);
      }
      else
      {
         EXPECT_EQ(42, new_value);

         oneway(7777);   // stop signal
      }

      first_ = false;
   }

   bool first_ = true;
};


struct SignalClient : simppl::dbus::Stub<Simple>
{
   SignalClient(simppl::dbus::Dispatcher& d)
    : simppl::dbus::Stub<Simple>(d, "ss")
   {
      connected >> [this](simppl::dbus::ConnectionState s)
      {
         EXPECT_EQ(simppl::dbus::ConnectionState::Connected, s);

         // like for attributes, attributes must be attached when the client is connected
         this->sig.attach() >> [this](int value)
         {
            this->handleSignal(value);
         };

         this->sig2.attach() >> [this](int value1, int value2)
         {
            this->handleSignal2(value1, value2);
         };

         this->oneway(100);
      };
   }


   void handleSignal(int value)
   {
      ++count_;

      EXPECT_EQ(start_++, value);

      // send stopsignal if appropriate
      if (start_ == 110)
         start_ = 7777;

      // trigger again
      oneway(start_);
   }

   void handleSignal2(int value1, int value2)
   {
      ++count2_;

      EXPECT_EQ(value1, -value2);
   }

   int start_ = 100;
   int count_ = 0;
   int count2_ = 0;
};


struct Server : simppl::dbus::Skeleton<Simple>
{
   Server(simppl::dbus::Dispatcher& d, const char* rolename)
    : simppl::dbus::Skeleton<Simple>(d, rolename)
   {
      // initialize attribute
      data = 4711;

      // initialize handlers
      hello >> [this]()
      {
         this->respond_with(hello());
      };


      hello_wait_for_some_time >> [this]()
      {
          std::this_thread::sleep_for(200ms);
          this->respond_with(hello_wait_for_some_time());
      };


      oneway >> [this](int i)
      {
         ++this->count_oneway_;

         if (i == 7777)
         {
            this->disp().stop();
         }
         else if (i < 100)
         {
            EXPECT_EQ(42, i);
            this->data = 42;
         }
         else
         {
            this->sig.notify(i);
            this->sig2.notify(i, -i);
         }
      };


      add >> [this](int i, double d)
      {
         this->respond_with(add(i*d));
      };


      echo >> [this](int i, double d)
      {
         this->respond_with(echo(i, d));
      };

      echo_wstring >> [this](const std::wstring& str)
      {
         this->respond_with(echo_wstring(str));
      };

      echo_wchart >> [this](wchar_t* str)
      {
         this->respond_with(echo_wchart(str));

         // clean up pointer
         simppl::dbus::detail::Deserializer::free(str);
      };
      
      test_rvo >> [this]()
      {
         Complex c;
         c.str1 = "Hello World";
         c.str2 = "Super";
         
         respond_with(test_rvo(c));
      };

   }

   int count_oneway_ = 0;
};


}   // anonymous namespace


TEST(Simple, methods)
{
   simppl::dbus::Dispatcher d("bus:session");
   Client c(d);
   Server s(d, "s");

   d.run();
}


TEST(Simple, signal)
{
   simppl::dbus::Dispatcher d("bus:session");
   SignalClient c(d);
   Server s(d, "ss");

   d.run();

   EXPECT_EQ(c.count_, 10);
   EXPECT_GT(c.count_, 0);
}


TEST(Simple, attribute)
{
   simppl::dbus::Dispatcher d("bus:session");
   PropertyClient c(d);
   Server s(d, "sa");

   d.run();
}


TEST(Simple, blocking)
{
   simppl::dbus::Dispatcher d("bus:session");

   std::thread t([](){
      simppl::dbus::Dispatcher d("bus:session");
      Server s(d, "sb");
      d.run();

      EXPECT_EQ(4, s.count_oneway_);
   });

   simppl::dbus::Stub<Simple> stub(d, "sb");

   // wait for server to get ready
   std::this_thread::sleep_for(200ms);

   stub.oneway(101);
   stub.oneway(102);
   stub.oneway(103);

   double result;
   result = stub.add(42, 0.5);

   EXPECT_GT(21.01, result);
   EXPECT_LT(20.99, result);

   int i;
   double x;
   std::tie(i, x) = stub.echo(42, 0.5);

   EXPECT_EQ(42, i);
   EXPECT_GT(0.51, x);
   EXPECT_LT(0.49, x);

   std::tuple<int, double> rslt;
   rslt = stub.echo(42, 0.5);

   EXPECT_EQ(42, std::get<0>(rslt));
   EXPECT_GT(0.51, std::get<1>(rslt));
   EXPECT_LT(0.49, std::get<1>(rslt));

   int dv = -1;
   dv = stub.data.get();
   EXPECT_EQ(4711, dv);

   std::wstring rslt_str = stub.echo_wstring(L"Hello world");
   EXPECT_EQ(0, rslt_str.compare(L"Hello world"));

   // different calling styles for pointers
   {
      const wchar_t* text = L"Hello world";
      wchar_t* rslt_p = stub.echo_wchart(text);
      EXPECT_EQ(0, ::wcscmp(rslt_p, L"Hello world"));

      simppl::dbus::detail::Deserializer::free(rslt_p);
   }

   {
      wchar_t* rslt_p = stub.echo_wchart(L"Hello world");
      EXPECT_EQ(0, ::wcscmp(rslt_p, L"Hello world"));

      simppl::dbus::detail::Deserializer::free(rslt_p);
   }

   {
      wchar_t text[16];
      wcscpy(text, L"Hello world");
      wchar_t* rslt_p = stub.echo_wchart(text);
      EXPECT_EQ(0, ::wcscmp(rslt_p, L"Hello world"));

      simppl::dbus::detail::Deserializer::free(rslt_p);
   }

   {
      wchar_t text[16];
      wcscpy(text, L"Hello world");
      wchar_t* tp = text;
      wchar_t* rslt_p = stub.echo_wchart(tp);
      EXPECT_EQ(0, ::wcscmp(rslt_p, L"Hello world"));

      simppl::dbus::detail::Deserializer::free(rslt_p);
   }

   stub.oneway(7777);   // stop server
   t.join();
}


TEST(Simple, disconnect)
{
   simppl::dbus::Dispatcher clientd;

   DisconnectClient c(clientd);

   {
      simppl::dbus::Dispatcher* serverd = new simppl::dbus::Dispatcher("bus:session");
      Server* s = new Server(*serverd, "s");

      std::thread serverthread([serverd, s](){
         serverd->run();
         delete s;
         delete serverd;
      });

      clientd.run();

      serverthread.join();
   }
}


TEST(Simple, cancel)
{
   simppl::dbus::Dispatcher clientd;

   CancelClient c(clientd);

   {
      simppl::dbus::Dispatcher* serverd = new simppl::dbus::Dispatcher("bus:session");
      Server* s = new Server(*serverd, "s");

      std::thread serverthread([serverd, s](){
         serverd->run();

         delete s;
         delete serverd;
      });

      clientd.run();

      serverthread.join();
   }
}


TEST(Simple, return_value_optimization)
{
   simppl::dbus::Dispatcher d("bus:session");
   
   std::thread t([](){
      simppl::dbus::Dispatcher d("bus:session");
      Server s(d, "rvo");
      d.run();
   });

   simppl::dbus::Stub<Simple> stub(d, "rvo");

   // wait for server to get ready
   std::this_thread::sleep_for(200ms);

   // make sure the return value does not create copy operations
   auto rc = stub.test_rvo();
   
   EXPECT_EQ(rc.str1, "Hello World");
   
   // no copy operation took place
   EXPECT_EQ(0, test::Complex::rvo_count);
   
   stub.oneway(7777);   // stop server
   t.join();
}
