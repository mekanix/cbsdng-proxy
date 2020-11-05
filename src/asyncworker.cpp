#include <iostream>
#include <vector>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/event.h>
#include <libutil.h>

#include "cbsdng/daemon/asyncworker.h"


#define READ_END 0
#define WRITE_END 1


bool AsyncWorker::quit = false;
std::mutex AsyncWorker::mutex;
std::condition_variable AsyncWorker::condition;
std::list<AsyncWorker *> AsyncWorker::finished;
static auto finishedThread = std::thread(&AsyncWorker::removeFinished);


AsyncWorker::AsyncWorker(const int &cl)
  : client{cl}
{
  t = std::thread(&AsyncWorker::_process, this);
}


AsyncWorker::~AsyncWorker()
{
  t.join();
  cleanup();
}


void AsyncWorker::execute(const Message &m)
{
  char prefix = 'j';
  int child;
  int noc = m.type() & Type::NOCOLOR;
  if (noc == 1)
  {
    std::stringstream nocolor;
    nocolor << noc;
    const auto data = nocolor.str();
    if (data.size() > 0)
    {
      setenv("NOCOLOR", data.data(), 1);
    }
  }
  int bhyve = m.type() & Type::BHYVE;
  if (bhyve > 0) { prefix = 'b'; }

  auto pid = forkpty(&child, nullptr, nullptr, nullptr);
  if (pid < 0)
  {
    std::cerr << "Failed to fork()\n";
    return;
  }
  else if (pid == 0) // child
  {
    std::string command = prefix + m.payload();
    std::string raw_command = "cbsd " + command;
    std::vector<char *> args;
    char *token = strtok((char *)raw_command.data(), " ");
    args.push_back(token);
    while ((token = strtok(nullptr, " ")) != nullptr)
    {
      args.push_back(token);
    }
    execvp(args[0], args.data());
  }
  else // parent
  {
    struct kevent events[2];
    struct kevent tevent;
    int kq = kqueue();
    if (kq == -1)
    {
      std::cerr << "kqueue: " << strerror(errno) << '\n';
    }
    EV_SET(events, child, EVFILT_READ, EV_ADD | EV_CLEAR, NOTE_READ, 0, nullptr);
    EV_SET(events + 1, client.raw(), EVFILT_READ, EV_ADD | EV_CLEAR, NOTE_READ, 0, nullptr);
    int rc = kevent(kq, events, 2, nullptr, 0, nullptr);
    if (rc == -1)
    {
      std::cerr << "kevent register: " << strerror(errno) << '\n';
    }
    while (true)
    {
      rc = kevent(kq, nullptr, 0, &tevent, 1, nullptr);
      if (rc == -1 || tevent.data == 0) { break; }
      Message m;
      if (tevent.ident == child)
      {
        char buffer[tevent.data + 1];
        rc = read(tevent.ident, buffer, tevent.data);
        if (rc <= 0) { continue; }
        buffer[rc] = '\0';
        m.data(0, 0, buffer);
        client << m;
      }
      else
      {
        client >> m;
        const auto &payload = m.payload();
        write(child, payload.data(), payload.size());
      }
    }
    int st;
    waitpid(pid, &st, 0);
  }
}


void AsyncWorker::_process()
{
  Message m;
  client >> m;
  execute(m);
  {
    std::unique_lock<std::mutex> lock(mutex);
    finished.push_back(this);
  }
  condition.notify_one();
}


void AsyncWorker::removeFinished()
{
  while (true)
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [] { return !finished.empty(); });
    auto worker = finished.front();
    finished.pop_front();
    if (worker != nullptr)
    {
      delete worker;
    }
    if (quit && finished.empty())
    {
      return;
    }
  }
}


void AsyncWorker::terminate()
{
  quit = true;
  {
    std::unique_lock<std::mutex> lock(mutex);
    finished.push_back(nullptr);
  }
  condition.notify_all();
}


void AsyncWorker::wait() { finishedThread.join(); }
void AsyncWorker::cleanup() { client.cleanup(); }
