/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2009,2010  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include <string.h>
#include <pthread.h>
#include <algorithm>
#include "compat.h"
#include <SDL.h>
#include "abc.h"
#include "flashdisplay.h"
#include "flashevents.h"
#include "swf.h"
#include "logger.h"
#include "actions.h"
#include "streams.h"
#include "asobjects.h"
#include "textfile.h"
#include "class.h"
#include "netutils.h"

#include <GL/glew.h>
#include <curl/curl.h>
extern "C" {
#include <libavcodec/avcodec.h>
}
#ifndef WIN32
#include <GL/glx.h>
#endif

using namespace std;
using namespace lightspark;

extern TLSDATA SystemState* sys;
extern TLSDATA RenderThread* rt;
extern TLSDATA ParseThread* pt;

SWF_HEADER::SWF_HEADER(istream& in)
{
	in >> Signature[0] >> Signature[1] >> Signature[2];

	in >> Version >> FileLength;
	if(Signature[0]=='F' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(LOG_NO_INFO, "Uncompressed SWF file: Version " << (int)Version << " Length " << FileLength);
	}
	else if(Signature[0]=='C' && Signature[1]=='W' && Signature[2]=='S')
	{
		LOG(LOG_NO_INFO, "Compressed SWF file: Version " << (int)Version << " Length " << FileLength);
	}
	else
	{
		LOG(LOG_NO_INFO,"No SWF file signature found");
		abort();
	}
	pt->version=Version;
	in >> FrameSize >> FrameRate >> FrameCount;
	LOG(LOG_NO_INFO,"FrameRate " << (FrameRate/256) << '.' << (FrameRate%256));
	float frameRate=FrameRate;
	frameRate/=256;

	pt->root->setFrameRate(frameRate);
	//TODO: setting render rate should be done when the clip is added to the displaylist
	sys->setRenderRate(frameRate);
	pt->root->version=Version;
	pt->root->fileLenght=FileLength;
}

RootMovieClip::RootMovieClip(LoaderInfo* li):initialized(false),frameRate(0),toBind(false)
{
	root=this;
	sem_init(&mutex,0,1);
	sem_init(&sem_frames,0,1);
	sem_init(&new_frame,0,0);
	sem_init(&sem_valid_size,0,0);
	sem_init(&sem_valid_rate,0,0);
	loaderInfo=li;
	//Reset framesLoaded, as there are still not available
	framesLoaded=0;

	//Root movie clip always has a linked object
	obj=new ASObject;
}

RootMovieClip::~RootMovieClip()
{
	sem_destroy(&mutex);
	sem_destroy(&sem_frames);
	sem_destroy(&new_frame);
	sem_destroy(&sem_valid_rate);
	sem_destroy(&sem_valid_size);
}

void RootMovieClip::bindToName(const tiny_string& n)
{
	assert(toBind==false);
	toBind=true;
	bindName=n;
}

SystemState::SystemState():RootMovieClip(NULL),showProfilingData(false),shutdown(false),error(false),currentVm(NULL),
	cur_input_thread(NULL),cur_render_thread(NULL),useInterpreter(true),useJit(false),downloadManager(NULL)
{
	//Do needed global initialization
	curl_global_init(CURL_GLOBAL_ALL);
	avcodec_register_all();

	//Create the thread pool
	sys=this;
	sem_init(&terminated,0,0);

	//Get starting time
	threadPool=new ThreadPool(this);
	timerThread=new TimerThread(this);
	loaderInfo=Class<LoaderInfo>::getInstanceS(true);
	stage=Class<Stage>::getInstanceS(true);
	startTime=compat_msectiming();
}

void SystemState::setUrl(const tiny_string& url)
{
	loaderInfo->url=url;
	loaderInfo->loaderURL=url;
}

void SystemState::parseParameters(istream& i)
{
	ASObject* ret=new ASObject;
	while(!i.eof())
	{
		string name,value;
		getline(i,name);
		getline(i,value);

		ret->setVariableByQName(name.c_str(),"",Class<ASString>::getInstanceS(true,value)->obj);
	}
	setParameters(ret);
}

void SystemState::setParameters(ASObject* p)
{
	loaderInfo->obj->setVariableByQName("parameters","",p);
}

SystemState::~SystemState()
{
	assert(shutdown);
	timerThread->stop();
	delete threadPool;
	delete downloadManager;
	delete currentVm;
	delete timerThread;

	//decRef all registered classes
	std::map<tiny_string, Class_base*>::iterator it=classes.begin();
	for(;it!=classes.end();++it)
		it->second->decRef();

	if(obj)
	{
		obj->implementation=NULL;
		//TODO: check??
		delete obj;
	}
}

void SystemState::setShutdownFlag()
{
	sem_wait(&mutex);
	shutdown=true;
	if(currentVm)
		currentVm->addEvent(NULL,new ShutdownEvent());

	sem_post(&terminated);
	sem_post(&mutex);
}

void SystemState::wait()
{
	sem_wait(&terminated);
}

float SystemState::getRenderRate()
{
	return renderRate;
}

void SystemState::setRenderRate(float rate)
{
	if(renderRate>=rate)
		return;
	
	//The requested rate is higher, let's reschedule the job
	renderRate=rate;
	assert(cur_render_thread);
	removeJob(cur_render_thread);
	addTick(1000/rate,cur_render_thread);
}

void SystemState::tick()
{
	cur_input_thread->broadcastEvent("enterFrame");
 	sem_wait(&mutex);
	list<ThreadProfile>::iterator it=profilingData.begin();
	for(;it!=profilingData.end();it++)
		it->tick();
	sem_post(&mutex);
}

void SystemState::addJob(IThreadJob* j)
{
	threadPool->addJob(j);
}

void SystemState::addTick(uint32_t tickTime, ITickJob* job)
{
	timerThread->addTick(tickTime,job);
}

void SystemState::addWait(uint32_t waitTime, ITickJob* job)
{
	timerThread->addWait(waitTime,job);
}

bool SystemState::removeJob(ITickJob* job)
{
	return timerThread->removeJob(job);
}

ThreadProfile* SystemState::allocateProfiler(const lightspark::RGB& color)
{
	sem_wait(&mutex);
	profilingData.push_back(ThreadProfile(color,100));
	ThreadProfile* ret=&profilingData.back();
	sem_post(&mutex);
	return ret;
}

void ThreadProfile::setTag(const std::string& t)
{
	Locker locker(mutex);
	if(data.empty())
		data.push_back(ProfilingData(tickCount,0));
	
	data.back().tag=t;
}

void ThreadProfile::accountTime(uint32_t time)
{
	Locker locker(mutex);
	if(data.empty() || data.back().index!=tickCount)
		data.push_back(ProfilingData(tickCount, time));
	else
		data.back().timing+=time;
}

void ThreadProfile::tick()
{
	Locker locker(mutex);
	tickCount++;
	//Purge first sample if the second is already old enough
	if(data.size()>2 && (tickCount-data[1].index)>uint32_t(len))
	{
		if(!data[0].tag.empty() && data[1].tag.empty())
			data[0].tag.swap(data[1].tag);
		data.pop_front();
	}
}

void ThreadProfile::plot(uint32_t maxTime, FTFont* font)
{
	if(data.size()<=1)
		return;

	Locker locker(mutex);
	RECT size=sys->getFrameSize();
	int width=size.Xmax/20;
	int height=size.Ymax/20;
	
	int32_t start=tickCount-len;
	if(int32_t(data[0].index-start)>0)
		start=data[0].index;
	
	glPushAttrib(GL_TEXTURE_BIT | GL_LINE_BIT);
	glColor3ub(color.Red,color.Green,color.Blue);
	glDisable(GL_TEXTURE_2D);
	glLineWidth(2);

	glBegin(GL_LINE_STRIP);
		for(unsigned int i=0;i<data.size();i++)
		{
			int32_t relx=int32_t(data[i].index-start)*width/len;
			glVertex2i(relx,data[i].timing*height/maxTime);
		}
	glEnd();
	glPopAttrib();
	
	//Draw tags
	string* curTag=NULL;
	int curTagX=0;
	int curTagY=maxTime;
	int curTagLen=0;
	int curTagH=0;
	for(unsigned int i=0;i<data.size();i++)
	{
		int32_t relx=int32_t(data[i].index-start)*width/len;
		if(!data[i].tag.empty())
		{
			//New tag, flush the old one if present
			if(curTag)
				font->Render(curTag->c_str() ,-1,FTPoint(curTagX,max(curTagY-curTagH,0)));
			//Measure tag
			FTBBox tagBox=font->BBox(data[i].tag.c_str(),-1);
			curTagLen=(tagBox.Upper()-tagBox.Lower()).X();
			curTagH=(tagBox.Upper()-tagBox.Lower()).Y();
			curTag=&data[i].tag;
			curTagX=relx;
			curTagY=maxTime;
		}
		if(curTag)
		{
			if(relx<(curTagX+curTagLen))
				curTagY=min(curTagY,data[i].timing*height/maxTime);
			else
			{
				//Tag is before this sample
				font->Render(curTag->c_str() ,-1,FTPoint(curTagX,max(curTagY-curTagH,0)));
				curTag=NULL;
			}
		}
	}
}

ParseThread::ParseThread(RootMovieClip* r,istream& in):f(in),parsingTarget(r)
{
	root=r;
	sem_init(&ended,0,0);
}

void ParseThread::execute()
{
	pt=this;
	try
	{
		SWF_HEADER h(f);
		root->setFrameSize(h.getFrameSize());
		root->setFrameCount(h.FrameCount);

		TagFactory factory(f);
		bool done=false;
		bool empty=true;
		while(!done)
		{
			Tag* tag=factory.readTag();
			switch(tag->getType())
			{
				case END_TAG:
				{
					LOG(LOG_NO_INFO,"End of parsing @ " << f.tellg());
					if(!empty)
						root->commitFrame(false);
					else
						root->revertFrame();
					done=true;
					break;
				}
				case DICT_TAG:
				{
					DictionaryTag* d=static_cast<DictionaryTag*>(tag);
					d->setLoadedFrom(root);
					root->addToDictionary(d);
					break;
				}
				case DISPLAY_LIST_TAG:
					root->addToFrame(static_cast<DisplayListTag*>(tag));
					empty=false;
					break;
				case SHOW_TAG:
					root->commitFrame(true);
					empty=true;
					break;
				case CONTROL_TAG:
					root->addToFrame(static_cast<ControlTag*>(tag));
					empty=false;
					break;
				case TAG:
					//Not yet implemented tag, ignore it
					break;
			}
			if(sys->shutdown)
				pthread_exit(0);
		}
	}
	catch(const char* s)
	{
		LOG(LOG_ERROR,"Exception caught: " << s);
		abort();
	}
	root->check();
	pt=NULL;

	sem_post(&ended);
}

void ParseThread::wait()
{
	sem_wait(&ended);
}

void RenderThread::wait()
{
	if(terminated)
		return;
	//Signal potentially blocing semaphore
	sem_post(&render);
	int ret=pthread_join(t,NULL);
	assert(ret==0);
	terminated=true;
}

InputThread::InputThread(SystemState* s,ENGINE e, void* param):m_sys(s),terminated(false)
{
	LOG(LOG_NO_INFO,"Creating input thread");
	sem_init(&sem_listeners,0,1);
	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
#ifndef WIN32
	else
	{
		npapi_params=(NPAPI_params*)param;
		pthread_create(&t,NULL,(thread_worker)npapi_worker,this);
	}
#endif
}

InputThread::~InputThread()
{
	wait();
}

void InputThread::wait()
{
	if(terminated)
		return;
	pthread_join(t,NULL);
	terminated=true;
}

#ifndef WIN32
void* InputThread::npapi_worker(InputThread* th)
{
/*	sys=th->m_sys;
	NPAPI_params* p=(NPAPI_params*)in_ptr;
//	Display* d=XOpenDisplay(NULL);
	XSelectInput(p->display,p->window,PointerMotionMask|ExposureMask);

	XEvent e;
	while(XWindowEvent(p->display,p->window,PointerMotionMask|ExposureMask, &e))
	{
		exit(-1);
	}*/
	return NULL;
}
#endif

void* InputThread::sdl_worker(InputThread* th)
{
	sys=th->m_sys;
	SDL_Event event;
	while(SDL_WaitEvent(&event))
	{
		switch(event.type)
		{
			case SDL_KEYDOWN:
			{
				switch(event.key.keysym.sym)
				{
					case SDLK_q:
						sys->setShutdownFlag();
						if(sys->currentVm)
							LOG(LOG_CALLS,"We still miss " << sys->currentVm->getEventQueueSize() << " events");
						pthread_exit(0);
						break;
					case SDLK_s:
						sys->state.stop_FP=true;
						break;
					case SDLK_p:
						sys->showProfilingData=!sys->showProfilingData;
						break;
					//Ignore any other keystrokes
					default:
						break;
				}
				break;
			}
			case SDL_MOUSEBUTTONDOWN:
			{

				float selected=sys->cur_render_thread->getIdAt(event.button.x,event.button.y);
				if(selected==0)
					break;

				sem_wait(&th->sem_listeners);

				selected--;
				int index=int(th->listeners.count("")*selected);
				
				pair< multimap<tiny_string, EventDispatcher*>::const_iterator,
					multimap<tiny_string, EventDispatcher*>::const_iterator > range=
					th->listeners.equal_range("");

				//Get the selected item
				multimap<tiny_string, EventDispatcher*>::const_iterator it=range.first;
				while(index)
				{
					it++;
					index--;
				}

				//Add event to the event queue
				sys->currentVm->addEvent(it->second,new Event("mouseDown"));

				sem_post(&th->sem_listeners);
				break;
			}
		}
	}
	return NULL;
}

void InputThread::addListener(const tiny_string& t, EventDispatcher* ob)
{
	sem_wait(&sem_listeners);
	LOG(LOG_TRACE,"Adding listener to " << t);

	//the empty string is the *any* event
	pair< multimap<tiny_string, EventDispatcher*>::const_iterator, 
		multimap<tiny_string, EventDispatcher*>::const_iterator > range=
		listeners.equal_range("");


	bool already_known=false;

	multimap<tiny_string, EventDispatcher*>::const_iterator it=range.first;
	int count=0;
	for(;it!=range.second;it++)
	{
		count++;
		if(it->second==ob)
		{
			already_known=true;
			break;
		}
	}
	range=listeners.equal_range(t);

	if(already_known)
	{
		//Check if this object is alreasy registered for this event
		it=range.first;
		int count=0;
		for(;it!=range.second;it++)
		{
			count++;
			if(it->second==ob)
			{
				LOG(LOG_TRACE,"Already added");
				sem_post(&sem_listeners);
				return;
			}
		}
	}
	else
		listeners.insert(make_pair("",ob));

	//Register the listener
	listeners.insert(make_pair(t,ob));
	count++;

	range=listeners.equal_range("");
	it=range.first;
	//Set a unique id for listeners in the range [0,1]
	//count is the number of listeners, this is correct so that no one gets 0
	float increment=1.0f/count;
	float cur=increment;
	for(;it!=range.second;it++)
	{
		if(it->second)
			it->second->setId(cur);
		cur+=increment;
	}

	sem_post(&sem_listeners);
}

void InputThread::broadcastEvent(const tiny_string& t)
{
	sem_wait(&sem_listeners);

	pair< multimap<tiny_string,EventDispatcher*>::const_iterator, 
		multimap<tiny_string, EventDispatcher*>::const_iterator > range=
		listeners.equal_range(t);

	for(;range.first!=range.second;range.first++)
		sys->currentVm->addEvent(range.first->second,Class<Event>::getInstanceS(true,t));

	sem_post(&sem_listeners);
}

RenderThread::RenderThread(SystemState* s,ENGINE e,void* params):m_sys(s),terminated(false),interactive_buffer(NULL),
					fbAcquired(false),frameCount(0),secsCount(0)
{
	LOG(LOG_NO_INFO,"RenderThread this=" << this);
	m_sys=s;
	sem_init(&render,0,0);
	if(e==SDL)
		pthread_create(&t,NULL,(thread_worker)sdl_worker,this);
#ifndef WIN32
	else if(e==NPAPI)
	{
		npapi_params=(NPAPI_params*)params;
		pthread_create(&t,NULL,(thread_worker)npapi_worker,this);
	}
	else if(e==GLX)
	{
		pthread_create(&t,NULL,(thread_worker)glx_worker,this);
	}

	clock_gettime(CLOCK_REALTIME,&ts);
#endif
}

RenderThread::~RenderThread()
{
	wait();
	sem_destroy(&render);
	delete[] interactive_buffer;
	LOG(LOG_NO_INFO,"~RenderThread this=" << this);
}

void RenderThread::glAcquireFramebuffer()
{
	assert(fbAcquired==false);
	fbAcquired=true;

	glDrawBuffer(GL_COLOR_ATTACHMENT1_EXT);
	glDisable(GL_BLEND);
	glClearColor(1,1,1,0);
	glClear(GL_COLOR_BUFFER_BIT);
}

void RenderThread::glBlitFramebuffer()
{
	assert(fbAcquired==true);
	fbAcquired=false;

	glPushMatrix();
	glEnable(GL_BLEND);
	glLoadIdentity();
	GLenum draw_buffers[]={GL_COLOR_ATTACHMENT0_EXT,GL_COLOR_ATTACHMENT2_EXT};
	glDrawBuffers(2,draw_buffers);

	glBindTexture(GL_TEXTURE_2D,rt->spare_tex);
	glColor4f(0,0,1,0);
	glBegin(GL_QUADS);
		glTexCoord2f(0,1);
		glVertex2i(0,0);
		glTexCoord2f(1,1);
		glVertex2i(rt->width,0);
		glTexCoord2f(1,0);
		glVertex2i(rt->width,rt->height);
		glTexCoord2f(0,0);
		glVertex2i(0,rt->height);
	glEnd();
	glPopMatrix();
}

#ifndef WIN32
void* RenderThread::npapi_worker(RenderThread* th)
{
	sys=th->m_sys;
	rt=th;
	NPAPI_params* p=th->npapi_params;

	RECT size=sys->getFrameSize();
	int swf_width=size.Xmax/20;
	int swf_height=size.Ymax/20;

	int window_width=p->width;
	int window_height=p->height;

	float scalex=window_width;
	scalex/=swf_width;
	float scaley=window_height;
	scaley/=swf_height;

	rt->width=window_width;
	rt->height=window_height;
	th->interactive_buffer=new float[window_width*window_height];
	unsigned int t2[3];
	sys=th->m_sys;
	rt=th;

	Display* d=XOpenDisplay(NULL);

    	int a,b;
    	Bool glx_present=glXQueryVersion(d,&a,&b);
	if(!glx_present)
	{
		printf("glX not present\n");
		return NULL;
	}
	int attrib[10]={GLX_BUFFER_SIZE,24,GLX_VISUAL_ID,p->visual,GLX_DEPTH_SIZE,24,None};
	GLXFBConfig* fb=glXChooseFBConfig(d, 0, attrib, &a);
	if(!fb)
	{
		attrib[0]=0;
		fb=glXChooseFBConfig(d, 0, NULL, &a);
		LOG(LOG_ERROR,"Falling back to no depth and no stencil");
	}
	int i;
	for(i=0;i<a;i++)
	{
		int id;
		glXGetFBConfigAttrib(d,fb[i],GLX_VISUAL_ID,&id);
		if(id==(int)p->visual)
		{
			printf("good id %x\n",id);
			break;
		}
	}
	th->mFBConfig=fb[i];
	XFree(fb);

	th->mContext = glXCreateNewContext(d,th->mFBConfig,GLX_RGBA_TYPE ,NULL,1);
	glXMakeContextCurrent(d, p->window, p->window, th->mContext);
	if(!glXIsDirect(d,th->mContext))
		printf("Indirect!!\n");

	th->commonGLInit(window_width, window_height, t2);

/*	XGCValues v;
	v.foreground=BlackPixel(d, 0);
	v.background=WhitePixel(d, 0);
	v.font=XLoadFont(d,"9x15");
	th->mGC=XCreateGC(d,p->window,GCForeground|GCBackground|GCFont,&v);*/

	glEnable(GL_TEXTURE_2D);
	try
	{
		while(1)
		{
			//Before starting rendering, cleanup all the request arrived in the meantime
			int fakeRenderCount=0;
			while(sem_trywait(&th->render)==0)
			{
				if(sys->shutdown)
					pthread_exit(0);
				fakeRenderCount++;
			}
			if(fakeRenderCount)
				LOG(LOG_NO_INFO,"Faking " << fakeRenderCount << " renderings");
			sem_wait(&th->render);
			if(sys->shutdown)
				pthread_exit(0);

			if(sys->error)
			{
				::abort();
				/*glXMakeContextCurrent(d, 0, 0, NULL);
				unsigned int h = p->height/2;
				//unsigned int w = 3 * p->width/4;
				int x = 0;
				int y = h/2;
				GC gc = XCreateGC(d, p->window, 0, NULL);
				const char *string = "ERROR";
				int l = strlen(string);
				int fmba = mFontInfo->max_bounds.ascent;
				int fmbd = mFontInfo->max_bounds.descent;
				int fh = fmba + fmbd;
				y += fh;
				x += 32;
				XDrawString(d, p->window, gc, x, y, string, l);
				XFreeGC(d, gc);*/
			}
			else
			{
				glXSwapBuffers(d,p->window);

				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, rt->fboId);
				//glReadBuffer(GL_COLOR_ATTACHMENT2_EXT);
				//glReadPixels(0,0,window_width,window_height,GL_RED,GL_FLOAT,th->interactive_buffer);

				glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);

				RGB bg=sys->getBackground();
				glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,0);
				glClearDepth(0xffff);
				glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
				glLoadIdentity();
				glScalef(scalex,scaley,1);
				
				sys->Render();

				glLoadIdentity();

				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
				glDrawBuffer(GL_BACK);

				glClearColor(0,0,0,1);
				glClear(GL_COLOR_BUFFER_BIT);

				glBindTexture(GL_TEXTURE_2D,t2[0]);
				glColor4f(0,0,1,0);
				glBegin(GL_QUADS);
					glTexCoord2f(0,1);
					glVertex2i(0,0);
					glTexCoord2f(1,1);
					glVertex2i(window_width,0);
					glTexCoord2f(1,0);
					glVertex2i(window_width,window_height);
					glTexCoord2f(0,0);
					glVertex2i(0,window_height);
				glEnd();
			}
		}
	}
	catch(const char* e)
	{
		LOG(LOG_ERROR,"Exception caught " << e);
		::abort();
	}
	glDisable(GL_TEXTURE_2D);
	delete p;
}
#endif

int RenderThread::load_program()
{
	GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	//GLuint v = glCreateShader(GL_VERTEX_SHADER);

	const char *fs = NULL;
	fs = dataFileRead(DATADIR "/lightspark.frag");
	if(fs==NULL)
		throw RunTimeException("Fragment shader code not found",sys->getOrigin().raw_buf());
	glShaderSource(f, 1, &fs,NULL);
	free((void*)fs);

/*	fs = dataFileRead(DATADIR "/lightspark.vert");
	glShaderSource(v, 1, &fs,NULL);
	free((void*)fs);*/

	char str[1024];
	int a;
	glCompileShader(f);
	glGetShaderInfoLog(f,1024,&a,str);
	printf("Fragment shader: %s\n",str);

/*	glCompileShader(v);
	glGetShaderInfoLog(v,1024,&a,str);
	printf("Vertex shader: %s\n",str);*/

	int ret = glCreateProgram();
	glAttachShader(ret,f);

	glLinkProgram(ret);
	return ret;
}

#ifndef WIN32
void* RenderThread::glx_worker(RenderThread* th)
{
	sys=th->m_sys;
	rt=th;

	RECT size=sys->getFrameSize();
	int width=size.Xmax/10;
	int height=size.Ymax/10;

	int attrib[20];
	attrib[0]=GLX_RGBA;
	attrib[1]=GLX_DOUBLEBUFFER;
	attrib[2]=GLX_DEPTH_SIZE;
	attrib[3]=24;
	attrib[4]=GLX_RED_SIZE;
	attrib[5]=8;
	attrib[6]=GLX_GREEN_SIZE;
	attrib[7]=8;
	attrib[8]=GLX_BLUE_SIZE;
	attrib[9]=8;
	attrib[10]=GLX_ALPHA_SIZE;
	attrib[11]=8;

	attrib[12]=None;

	XVisualInfo *vi;
	XSetWindowAttributes swa;
	Colormap cmap; 
	th->mDisplay = XOpenDisplay(0);
	vi = glXChooseVisual(th->mDisplay, DefaultScreen(th->mDisplay), attrib);

	int a;
	attrib[0]=GLX_VISUAL_ID;
	attrib[1]=vi->visualid;
	attrib[2]=GLX_DEPTH_SIZE;
	attrib[3]=24;
	attrib[4]=GLX_RED_SIZE;
	attrib[5]=8;
	attrib[6]=GLX_GREEN_SIZE;
	attrib[7]=8;
	attrib[8]=GLX_BLUE_SIZE;
	attrib[9]=8;
	attrib[10]=GLX_ALPHA_SIZE;
	attrib[11]=8;
	attrib[12]=GLX_DRAWABLE_TYPE;
	attrib[13]=GLX_PBUFFER_BIT;

	attrib[14]=None;
	GLXFBConfig* fb=glXChooseFBConfig(th->mDisplay, 0, attrib, &a);

	//We create a pair of context, window and offscreen
	th->mContext = glXCreateContext(th->mDisplay, vi, 0, GL_TRUE);

	attrib[0]=GLX_PBUFFER_WIDTH;
	attrib[1]=width;
	attrib[2]=GLX_PBUFFER_HEIGHT;
	attrib[3]=height;
	attrib[4]=None;
	th->mPbuffer = glXCreatePbuffer(th->mDisplay, fb[0], attrib);

	XFree(fb);

	cmap = XCreateColormap(th->mDisplay, RootWindow(th->mDisplay, vi->screen), vi->visual, AllocNone);
	swa.colormap = cmap; 
	swa.border_pixel = 0; 
	swa.event_mask = StructureNotifyMask; 

	th->mWindow = XCreateWindow(th->mDisplay, RootWindow(th->mDisplay, vi->screen), 100, 100, width, height, 0, vi->depth, 
			InputOutput, vi->visual, CWBorderPixel|CWEventMask|CWColormap, &swa);
	
	XMapWindow(th->mDisplay, th->mWindow);
	glXMakeContextCurrent(th->mDisplay, th->mWindow, th->mWindow, th->mContext); 

	glEnable( GL_DEPTH_TEST );
	glDepthFunc(GL_LEQUAL);

	glViewport(0,0,width,height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,width,height,0,-100,0);
	glScalef(0.1,0.1,1);

	glMatrixMode(GL_MODELVIEW);

	unsigned int t;
	glGenTextures(1,&t);

	glBindTexture(GL_TEXTURE_1D,t);

	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_WRAP_S,GL_CLAMP);

	//Load fragment shaders
	rt->gpu_program=load_program();
	int tex=glGetUniformLocation(rt->gpu_program,"g_tex");
	glUniform1i(tex,0);
	glUseProgram(rt->gpu_program);

	float* buffer=new float[640*240];
	try
	{
		while(1)
		{
			sem_wait(&th->render);
			glXSwapBuffers(th->mDisplay,th->mWindow);
			RGB bg=sys->getBackground();
			glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,0);
			glClearDepth(0xffff);
			glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
			glLoadIdentity();

			sys->Render();

			if(sys->shutdown)
			{
				delete[] buffer;
				pthread_exit(0);
			}
		}
	}
	catch(const char* e)
	{
		LOG(LOG_ERROR, "Exception caught " << e);
		delete[] buffer;
		::abort();
	}
}
#endif

float RenderThread::getIdAt(int x, int y)
{
	return interactive_buffer[y*width+x];
}

void RootMovieClip::initialize()
{
	if(!initialized && sys->currentVm)
	{
		initialized=true;
		//Let's see if we have to bind the root movie clip itself
		if(bindName.len())
			sys->currentVm->addEvent(NULL,new BindClassEvent(this,bindName));
		//Now signal the completion for this root
		sys->currentVm->addEvent(loaderInfo,Class<Event>::getInstanceS(true,"init"));
		//Wait for handling of all previous events
		SynchronizationEvent* sync=new SynchronizationEvent;
		sys->currentVm->addEvent(NULL, sync);
		sync->wait();
	}
}

bool RootMovieClip::getBounds(number_t& xmin, number_t& xmax, number_t& ymin, number_t& ymax) const
{
	RECT f=getFrameSize();
	xmin=0;
	ymin=0;
	xmax=f.Xmax;
	ymax=f.Ymax;
	return true;
}

void RootMovieClip::Render()
{
	sem_wait(&sem_frames);
	while(1)
	{
		//Check if the next frame we are going to play is available
		if(state.next_FP<frames.size())
			break;

		sem_post(&sem_frames);
		sem_wait(&new_frame);
		sem_wait(&sem_frames);
	}

	MovieClip::Render();
	sem_post(&sem_frames);
}

void RenderThread::commonGLInit(int width, int height, unsigned int t2[3])
{
	//Now we can initialize GLEW
	GLenum err = glewInit();
	if (GLEW_OK != err)
	{
		LOG(LOG_ERROR,"Cannot initialize GLEW");
		abort();
	}

	//Load fragment shaders
	rt->gpu_program=load_program();

	int tex=glGetUniformLocation(rt->gpu_program,"g_tex1");
	glUniform1i(tex,0);

	glUseProgram(rt->gpu_program);

	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	
	glViewport(0,0,width,height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,width,0,height,-100,0);
	//glTranslatef(0,height/2,0);

	glMatrixMode(GL_MODELVIEW);

	GLuint dt;
	glGenTextures(1,&dt);
	rt->data_tex=dt;

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D,dt);

	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
 	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

 	glGenTextures(3,t2);
	glBindTexture(GL_TEXTURE_2D,t2[0]);

	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

	glBindTexture(GL_TEXTURE_2D,t2[1]);
	rt->spare_tex=t2[1];

	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

	glBindTexture(GL_TEXTURE_2D,t2[2]);

	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
	glTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	
	// create a framebuffer object
	glGenFramebuffersEXT(1, &rt->fboId);
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, rt->fboId);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D, t2[0], 0);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT,GL_TEXTURE_2D, t2[1], 0);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT2_EXT,GL_TEXTURE_2D, t2[2], 0);
	
	// check FBO status
	GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	if(status != GL_FRAMEBUFFER_COMPLETE_EXT)
	{
		//cout << status << endl;
		abort();
	}
}

void* RenderThread::sdl_worker(RenderThread* th)
{
	sys=th->m_sys;
	rt=th;
	RECT size=sys->getFrameSize();
	int width=size.Xmax/20;
	int height=size.Ymax/20;
	rt->width=width;
	rt->height=height;
	th->interactive_buffer=new float[width*height];
	unsigned int t2[3];
	SDL_Init(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
	SDL_GL_SetAttribute( SDL_GL_SWAP_CONTROL, 0 );
	SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, 1); 
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

	SDL_SetVideoMode( width, height, 24, SDL_OPENGL );
	th->commonGLInit(width, height, t2);

	ThreadProfile* profile=sys->allocateProfiler(RGB(200,0,0));
	profile->setTag("Render");
	FTTextureFont font("/usr/share/fonts/truetype/ttf-liberation/LiberationSerif-Regular.ttf");
	if(font.Error())
		throw RunTimeException("Unable to load font",sys->getOrigin().raw_buf());
	
	font.FaceSize(20);
	try
	{
		//Texturing must be enabled otherwise no tex coord will be sent to the shader
		glEnable(GL_TEXTURE_2D);
		while(1)
		{
			sem_wait(&th->render);
			Chronometer chronometer;

			SDL_GL_SwapBuffers( );

			//Before starting rendering, cleanup all the request arrived in the meantime
			int fakeRenderCount=0;
			while(sem_trywait(&th->render)==0)
			{
				if(sys->shutdown)
					pthread_exit(0);
				fakeRenderCount++;
			}

			if(fakeRenderCount)
				LOG(LOG_NO_INFO,"Faking " << fakeRenderCount << " renderings");

			if(sys->shutdown)
				pthread_exit(0);
			SDL_PumpEvents();

			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, rt->fboId);
			//glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
			//glReadPixels(0,0,width,height,GL_RED,GL_FLOAT,th->interactive_buffer);

			glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);

			RGB bg=sys->getBackground();
			//glClearColor(bg.Red/255.0F,bg.Green/255.0F,bg.Blue/255.0F,1);
			glClearColor(1,1,1,1);
			glClear(GL_COLOR_BUFFER_BIT);
			glLoadIdentity();
			
			sys->Render();

			glFlush();

			glLoadIdentity();

			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
			glDrawBuffer(GL_BACK);

			glClearColor(0,0,0,1);
			glClear(GL_COLOR_BUFFER_BIT);

			glBindTexture(GL_TEXTURE_2D,t2[0]);
			glColor4f(0,0,1,0);
			glBegin(GL_QUADS);
				glTexCoord2f(0,0);
				glVertex2i(0,0);
				glTexCoord2f(1,0);
				glVertex2i(width,0);
				glTexCoord2f(1,1);
				glVertex2i(width,height);
				glTexCoord2f(0,1);
				glVertex2i(0,height);
			glEnd();

			if(sys->showProfilingData)
			{
				glUseProgram(0);
				
				//Draw bars
				glColor4f(0.7,0.7,0.7,0.7);
				glBegin(GL_LINES);
				for(int i=1;i<10;i++)
				{
					glVertex2i(0,(i*height/10));
					glVertex2i(width,(i*height/10));
				}
				glEnd();
				
				list<ThreadProfile>::iterator it=sys->profilingData.begin();
				for(;it!=sys->profilingData.end();it++)
					it->plot(1000000/sys->getFrameRate(),&font);
				glUseProgram(rt->gpu_program);
			}
			//Call glFlush to offload work on the GPU
			glFlush();
			profile->accountTime(chronometer.checkpoint());
		}
		glDisable(GL_TEXTURE_2D);
	}
	catch(const char* e)
	{
		LOG(LOG_ERROR, "Exception caught " << e);
		::abort();
	}
	return NULL;
}

void RenderThread::draw()
{
	sem_post(&render);
#ifdef CLOCK_REALTIME
	clock_gettime(CLOCK_REALTIME,&td);
	uint32_t diff=timeDiff(ts,td);
	if(diff>1000)
	{
		ts=td;
		LOG(LOG_NO_INFO,"FPS: " << dec << frameCount);
		//fps_prof->fps=frameCount;
		frameCount=0;
		secsCount++;
		//fps_profs.push_back(fps_profiling());
		//fps_prof=&fps_profs.back();
		/*if(secsCount>120)
		{
			LOG(LOG_NO_INFO,"Exiting");
			setShutdownFlag();
		}*/
	}
	else
		frameCount++;
#endif
}

void RenderThread::tick()
{
	draw();
}

void RootMovieClip::setFrameCount(int f)
{
	sem_wait(&sem_frames);
	totalFrames=f;
	state.max_FP=f;
	assert(cur_frame==&frames.back());
	frames.reserve(f);
	cur_frame=&frames.back();
	sem_post(&sem_frames);
}

void RootMovieClip::setFrameSize(const lightspark::RECT& f)
{
	frameSize=f;
	assert(f.Xmin==0 && f.Ymin==0);
	sem_post(&sem_valid_size);
}

lightspark::RECT RootMovieClip::getFrameSize() const
{
	//This is a sync semaphore the first time and then a mutex
	sem_wait(&sem_valid_size);
	lightspark::RECT ret=frameSize;
	sem_post(&sem_valid_size);
	return ret;
}

void RootMovieClip::setFrameRate(float f)
{
	frameRate=f;
	//Now frame rate is valid, start the rendering
	sys->addTick(1000/f,this);
	sem_post(&sem_valid_rate);
}

float RootMovieClip::getFrameRate() const
{
	//This is a sync semaphore the first time and then a mutex
	sem_wait(&sem_valid_rate);
	float ret=frameRate;
	sem_post(&sem_valid_rate);
	return ret;
}

void RootMovieClip::addToDictionary(DictionaryTag* r)
{
	sem_wait(&mutex);
	dictionary.push_back(r);
	sem_post(&mutex);
}

void RootMovieClip::addToFrame(DisplayListTag* t)
{
	sem_wait(&mutex);
	MovieClip::addToFrame(t);
	sem_post(&mutex);
}

void RootMovieClip::addToFrame(ControlTag* t)
{
	cur_frame->controls.push_back(t);
}

void RootMovieClip::commitFrame(bool another)
{
	sem_wait(&sem_frames);
	framesLoaded=frames.size();
	if(another)
	{
		frames.push_back(Frame());
		cur_frame=&frames.back();
	}
	else
		cur_frame=NULL;

	if(framesLoaded==1)
	{
		//Let's initialize the first frame of this movieclip
		bootstrap();
	}
	sem_post(&new_frame);
	sem_post(&sem_frames);
}

void RootMovieClip::revertFrame()
{
	sem_wait(&sem_frames);
	assert(frames.size() && framesLoaded==(frames.size()-1));
	frames.pop_back();
	cur_frame=NULL;
	sem_post(&sem_frames);
}

RGB RootMovieClip::getBackground()
{
	return Background;
}

void RootMovieClip::setBackground(const RGB& bg)
{
	Background=bg;
}

DictionaryTag* RootMovieClip::dictionaryLookup(int id)
{
	sem_wait(&mutex);
	list< DictionaryTag*>::iterator it = dictionary.begin();
	for(;it!=dictionary.end();it++)
	{
		if((*it)->getId()==id)
			break;
	}
	if(it==dictionary.end())
	{
		LOG(LOG_ERROR,"No such Id on dictionary " << id);
		abort();
	}
	DictionaryTag* ret=*it;
	sem_post(&mutex);
	return ret;
}

void RootMovieClip::tick()
{
	//Should go to the next frame
}

/*ASObject* RootMovieClip::getVariableByQName(const tiny_string& name, const tiny_string& ns, ASObject*& owner)
{
	sem_wait(&mutex);
	ASObject* ret=ASObject::getVariableByQName(name, ns, owner);
	sem_post(&mutex);
	return ret;
}

void RootMovieClip::setVariableByMultiname(multiname& name, ASObject* o)
{
	sem_wait(&mutex);
	ASObject::setVariableByMultiname(name,o);
	sem_post(&mutex);
}

void RootMovieClip::setVariableByQName(const tiny_string& name, const tiny_string& ns, ASObject* o)
{
	sem_wait(&mutex);
	ASObject::setVariableByQName(name,ns,o);
	sem_post(&mutex);
}

void RootMovieClip::setVariableByString(const string& s, ASObject* o)
{
	abort();
	//TODO: ActionScript2 support
	string sub;
	int f=0;
	int l=0;
	ASObject* target=this;
	for(l;l<s.size();l++)
	{
		if(s[l]=='.')
		{
			sub=s.substr(f,l-f);
			ASObject* next_target;
			if(f==0 && sub=="__Packages")
			{
				next_target=&sys->cur_render_thread->vm.Global;
				owner=&sys->cur_render_thread->vm.Global;
			}
			else
				next_target=target->getVariableByQName(sub.c_str(),"",owner);

			f=l+1;
			if(!owner)
			{
				next_target=new Package;
				target->setVariableByQName(sub.c_str(),"",next_target);
			}
			target=next_target;
		}
	}
	sub=s.substr(f,l-f);
	target->setVariableByQName(sub.c_str(),"",o);
}*/

long lightspark::timeDiff(timespec& s, timespec& d)
{
	timespec temp;
	if ((d.tv_nsec-s.tv_nsec)<0) {
		temp.tv_sec = d.tv_sec-s.tv_sec-1;
		temp.tv_nsec = 1000000000+d.tv_nsec-s.tv_nsec;
	} else {
		temp.tv_sec = d.tv_sec-s.tv_sec;
		temp.tv_nsec = d.tv_nsec-s.tv_nsec;
	}
	return temp.tv_sec*1000+(temp.tv_nsec)/1000000;
}

