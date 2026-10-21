// Own-code ABI fixture, not Valve code. Never loads Steam or uses an account.
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <cstddef>
#define API extern "C" __declspec(dllexport)
using Q=std::uint64_t;
struct Identity {int type,size; unsigned char data[128];};
struct Callback {int user,callback; void* param; int size;};
struct Completed {Q call;int callback;unsigned size;};
struct Created {int result;Q lobby;};
struct Entered {Q lobby;unsigned permissions;bool locked;unsigned response;};
struct Message {void* data;int size;int connection;Identity sender;int padding;Q connectionUser;long long received;long long number;void* freeData;void* release;int channel;int flags;Q user;};
static_assert(sizeof(Identity)==136 && sizeof(Callback)==24 && sizeof(Completed)==16);
static_assert(sizeof(Created)==16 && sizeof(Entered)==24 && offsetof(Entered,response)==16);
static_assert(offsetof(Message,sender)==16);
static constexpr Q lobby=480000123456ULL;
static int errors=0,init=0,leaves=0,shutdowns=0,releases=0,freed=0,phase=0;
static Completed completion{};static Created created{1,lobby};static Entered entered{lobby,0,false,1};
static std::string tag;static std::vector<unsigned char> payload;static Message* outstanding=nullptr;
static void require(bool ok){if(!ok)++errors;}
API const char* FOF_MockIdentity(){return "FOF_MOCK_NO_ACCOUNT";}
API int FOF_MockMetric(int metric){switch(metric){case 0:return errors;case 1:return leaves;case 2:return shutdowns;case 3:return releases;case 4:return freed;default:return -1;}}
API int SteamAPI_InitFlat(char* error){require(!init);init=1;if(error)*error=0;return 0;}
API void SteamAPI_Shutdown(){require(init&&outstanding==nullptr);init=0;++shutdowns;}
API void* SteamAPI_SteamUser_v023(){require(init);return (void*)1;}
API void* SteamAPI_SteamMatchmaking_v009(){require(init);return (void*)2;}
API void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002(){require(init);return (void*)3;}
API int SteamAPI_GetHSteamPipe(){require(init);return 4;}
API bool SteamAPI_ISteamUser_BLoggedOn(void* self){require(self==(void*)1);return true;}
API Q SteamAPI_ISteamUser_GetSteamID(void* self){require(self==(void*)1);return 101;}
API void SteamAPI_ManualDispatch_Init(){require(init);}
API void SteamAPI_ManualDispatch_RunFrame(int pipe){require(pipe==4);}
API Q SteamAPI_ISteamMatchmaking_CreateLobby(void* self,int type,int capacity){require(self==(void*)2&&type==1&&capacity==2);phase=0;completion={77,513,sizeof(Created)};return 77;}
API Q SteamAPI_ISteamMatchmaking_JoinLobby(void* self,Q id){require(self==(void*)2&&id==lobby);phase=0;completion={78,504,sizeof(Entered)};return 78;}
API bool SteamAPI_ManualDispatch_GetNextCallback(int pipe,Callback* message){
 require(pipe==4);static Completed unrelated{66,999,8};
 if(phase==0){*message={1,703,&unrelated,sizeof(unrelated)};++phase;return true;}
 if(phase==1){*message={1,703,&completion,sizeof(completion)};++phase;return true;}
 if(phase==2){*message={1,504,&entered,sizeof(entered)};++phase;return true;}return false;
}
API void SteamAPI_ManualDispatch_FreeLastCallback(int pipe){require(pipe==4);++freed;}
API bool SteamAPI_ManualDispatch_GetAPICallResult(int pipe,Q call,void* output,int size,int callback,bool* failed){
 bool valid=pipe==4&&call==completion.call&&size==(int)completion.size&&callback==completion.callback;
 require(valid);*failed=!valid;if(!valid)return false;
 std::memcpy(output,callback==513?(void*)&created:(void*)&entered,size);return true;
}
API void SteamAPI_ISteamMatchmaking_LeaveLobby(void* self,Q id){require(self==(void*)2&&id==lobby);++leaves;}
API Q SteamAPI_ISteamMatchmaking_GetLobbyOwner(void* self,Q id){require(self==(void*)2&&id==lobby);return 101;}
API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void* self,Q id){require(self==(void*)2&&id==lobby);return 2;}
API Q SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void* self,Q id,int index){require(self==(void*)2&&id==lobby&&index>=0&&index<2);return index?202:101;}
API bool SteamAPI_ISteamMatchmaking_SetLobbyData(void* self,Q id,const char* key,const char* value){require(self==(void*)2&&id==lobby&&!std::strcmp(key,"fof_native"));tag=value;return true;}
API const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void* self,Q id,const char* key){require(self==(void*)2&&id==lobby&&!std::strcmp(key,"fof_native"));return tag.c_str();}
API void SteamAPI_SteamNetworkingIdentity_SetSteamID64(Identity* identity,Q id){std::memset(identity,0,sizeof(*identity));identity->type=16;identity->size=8;std::memcpy(identity->data,&id,8);}
API Q SteamAPI_SteamNetworkingIdentity_GetSteamID64(const Identity* identity){require(identity->type==16&&identity->size==8);Q id=0;std::memcpy(&id,identity->data,8);return id;}
API bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void* self,const Identity* identity){require(self==(void*)3&&SteamAPI_SteamNetworkingIdentity_GetSteamID64(identity)==202);return true;}
API bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void* self,const Identity* identity){return SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(self,identity);}
API int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void* self,const Identity* identity,const void* data,unsigned size,int flags,int channel){
 require(self==(void*)3&&SteamAPI_SteamNetworkingIdentity_GetSteamID64(identity)==202&&flags==9&&channel==0&&size>=16&&size<=16400&&payload.empty());
 const auto* bytes=(const unsigned char*)data;payload.assign(bytes,bytes+size);return 1;
}
API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void* self,int channel,void** messages,int capacity){
 require(self==(void*)3&&channel==0&&capacity>0&&!outstanding);if(payload.empty())return 0;
 outstanding=new Message{};outstanding->data=new unsigned char[payload.size()];outstanding->size=(int)payload.size();
 std::memcpy(outstanding->data,payload.data(),payload.size());payload.clear();SteamAPI_SteamNetworkingIdentity_SetSteamID64(&outstanding->sender,202);messages[0]=outstanding;return 1;
}
API void SteamAPI_SteamNetworkingMessage_t_Release(Message* message){require(message==outstanding);delete[](unsigned char*)message->data;delete message;outstanding=nullptr;++releases;}
