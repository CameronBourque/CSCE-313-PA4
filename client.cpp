#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include "FIFOreqchannel.h"
#include <thread>
#include <mutex>
#include <unistd.h>
#include <cstdio>
#include <signal.h>
#include <time.h>
using namespace std;

//GLOBAL VARIABLES
HistogramCollection hc;
__int64_t iters;
__int64_t progress;

void sig_hdlr(int signo){
    if(signo == SIGALRM){
        system("clear");
        if(!hc.is_empty()){
            hc.print();
            cout << "\n\n\n\n" << endl;
        }
        if(iters > 0){
            int pct = ((double)progress / (double)iters) * 100;

            cout << "+----------------------------------------------------------------------------------------------------+" << endl;
            cout << "|";
            for(int i = 0; i < 100; i++){
                if(i <= pct){
                    cout << "#";
                }
                else{
                    cout << " ";
                }
            }
            cout << "|" << endl;
            cout << "+----------------------------------------------------------------------------------------------------+" << endl;
        }
    }
}

void file_thread_function(string filename, int m, BoundedBuffer* reqbuf, FIFORequestChannel* chan){
    string rpsfname = "recv/" + filename;
    char buf[1024];
    filemsg msg(0, 0);

    memcpy(buf, &msg, sizeof(msg));
    strcpy(buf + sizeof(msg), filename.c_str());
    chan->cwrite(buf, sizeof(msg) + filename.size() + 1);

    __int64_t flength;
    chan->cread(&flength, sizeof(flength));
    cout << flength << endl;

    FILE* fptr = fopen(rpsfname.c_str(), "wb");
    if(fptr == NULL){
        cout << "error: " << strerror(errno) << endl;

        return;
    }

    fseek(fptr, flength, SEEK_SET);
    fclose(fptr);

    filemsg* fmsg = (filemsg*)buf;
    __int64_t remlength = flength;

    while(remlength > 0){
        fmsg->length = min(remlength, (__int64_t)m);
        reqbuf->push(buf, sizeof(filemsg) + filename.size() + 1);
        fmsg->offset += fmsg->length;
        remlength -= fmsg->length;
        iters++;
    }
}

void patient_thread_function(int points, int patient, BoundedBuffer* reqbuf){
    datamsg msg(patient, 0.0, 1);
    for(int i = 0; i < points; i++){
        reqbuf->push((char*)&msg, sizeof(msg));

        msg.seconds += 0.004;
    }
}

void worker_thread_function(FIFORequestChannel* chan, BoundedBuffer* reqbuf, HistogramCollection* hc, int mem, mutex* m){
    char buf[1024];
    double rsp;
    bool running = true;
    char rspbuf[mem];

    while(running){
        int mSize = reqbuf->pop(buf, 1024);
        MESSAGE_TYPE* msg = (MESSAGE_TYPE *)buf;

        switch (*msg){
            case DATA_MSG:
                chan->cwrite(buf, sizeof(datamsg));
                chan->cread(&rsp, sizeof(rsp));
                hc->update(((datamsg*)msg)->person, rsp);
                break;

            case FILE_MSG: {
                filemsg *fmsg = (filemsg *) buf;
                string filename = (char *) (fmsg + 1);
                chan->cwrite(buf, sizeof(filemsg) + filename.size() + 1);
                chan->cread(rspbuf, mem);

                string rspfname = "recv/" + filename;
                FILE *fptr = fopen(rspfname.c_str(), "rb+");
                fseek(fptr, fmsg->offset, SEEK_SET);
                fwrite(rspbuf, 1, fmsg->length, fptr);
                fclose(fptr);

                m->lock();
                progress++;
                m->unlock();
                break;
            }

            case QUIT_MSG:
                chan->cwrite(msg, sizeof(MESSAGE_TYPE));
                running = false;
                delete chan;
                break;
        }
    }
}

//create new fifo channels for the threads to use
FIFORequestChannel* create_channel(FIFORequestChannel* main){
    char name [1024];
    MESSAGE_TYPE msg = NEWCHANNEL_MSG;
    main->cwrite((char*)&msg, sizeof(msg));
    main->cread(name, 1024);
    FIFORequestChannel* newch = new FIFORequestChannel(name, FIFORequestChannel::CLIENT_SIDE);
    return newch;
}

//main function
int main(int argc, char *argv[])
{
    int n = 0;    //default number of requests per "patient"
    int p = 0;     // number of patients [1,15]
    int w = 0;    //default number of worker threads
    int b = 20; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
	string filename = "";

	//set globals
	iters = 0;
	progress = 0;

    srand(time_t(NULL));

    int c;
    while((c = getopt(argc, argv, ":c:n:p:w:b:m:f:")) != -1) {
        switch (c) {
            case 'n':
                n = atoi(optarg);
                break;
            case 'p':
                p = atoi(optarg);
                break;
            case 'w':
                w = atoi(optarg);
                break;
            case 'b':
                b = atoi(optarg);
            case 'm':
                m = atoi(optarg);
                break;
            case 'f':
                filename = optarg;
                break;
                //is this needed??
            case ':':
                switch (optopt) {
                    case 'c':
                        //channel = true;
                        break;
                }
                break;
        }
    }

    int pid = fork();
    if (pid == 0){
		string memtoa = to_string(m);
        execl ("server", "server", "-m", (char*)memtoa.c_str(), (char *)NULL);
    }

	FIFORequestChannel* chan = new FIFORequestChannel("control", FIFORequestChannel::CLIENT_SIDE);
    BoundedBuffer request_buffer(b);

	//setup the histograms
	for(int i = 0; i < p; i++){
	                        //bins, start_ecg_val, end_ecg_val
	    Histogram* h = new Histogram(10, -2.0, 2.0);
	    hc.add(h);
	}


    //set up signal handler
    struct sigaction sa;
    sa.sa_handler = sig_hdlr;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_SIGINFO;

    sigaction(SIGALRM, &sa, NULL);

    //set up timer
    timer_t timer;
    struct sigevent sigev;
    sigev.sigev_notify = SIGEV_SIGNAL;
    sigev.sigev_signo = SIGALRM;
    sigev.sigev_value.sival_ptr = &hc; //test this
    timer_create(CLOCK_REALTIME, &sigev, &timer);
    struct itimerspec its;
    its.it_value.tv_sec = 2;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;
    timer_settime(timer, 0, &its, NULL);

	//setup worker channels
	FIFORequestChannel* wc [w];
	for(int i = 0; i < w; i++){
        wc[i] = create_channel(chan);
	}

    struct timeval start, end;
    gettimeofday (&start, 0);

    /* Start all threads here */
    thread patient[p];
    if(n > 0 && (p > 0 || p <= 15) && w > 0) {
        for (int i = 0; i < p; i++) {
            patient[i] = thread(&patient_thread_function, n, i + 1, &request_buffer);
        }
    }

    thread* filethread;
    if(filename.size() > 0 && w > 0) {
        filethread = new thread(&file_thread_function, filename, m, &request_buffer, chan);
    }

    mutex mtx;
    thread worker[w];
    for(int i = 0; i < w; i++){
        worker[i] = thread(&worker_thread_function, wc[i], &request_buffer, &hc, m, &mtx);
    }



	/* Join all threads here */
	if(n > 0 && (p > 0 || p <= 15) && w > 0) {
        for (int i = 0; i < p; i++) {
            patient[i].join();
        }
        cout << "Patient threads done" << endl;
    }

    if(filename.size() > 0) {
        filethread->join();
        cout << "File thread done" << endl;
    }

    MESSAGE_TYPE q = QUIT_MSG;
    for(int i = 0; i < w; i++){
        request_buffer.push((char*)&q, sizeof(q));
    }

    for(int i = 0; i < w; i++){
        worker[i].join();
    }
    cout << "Worker threads done" << endl;

    gettimeofday (&end, 0);
    // print the results
	hc.print ();
    int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
    int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
    cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

    //clean up main channel
    chan->cwrite ((char *) &q, sizeof (MESSAGE_TYPE));
    cout << "All Done!!!" << endl;
    delete chan;

    
}
