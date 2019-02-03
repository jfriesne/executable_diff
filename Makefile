# compilation flags used under any OS or compiler (may be appended to, below)
CXXFLAGS   += -Imuscle -DMUSCLE_SINGLE_THREAD_ONLY

# compilation flags that are specific to the gcc compiler (hard-coded)
GCCFLAGS    = -Wall -W -Wno-multichar

# flags to include when compiling the optimized version (with 'make optimized')
CCOPTFLAGS  = -g

# flags to include when linking (set per operating system, below)
LFLAGS      = 

# libraries to include when linking (set per operating system, below)
LIBS        = 

# names of the executables to compile
EXECUTABLES = executable_diff

# object files to include in all executables
OBJFILES = SignalHandlerSession.o SignalMultiplexer.o Message.o AbstractMessageIOGateway.o MessageIOGateway.o Directory.o FilePathInfo.o MiscUtilityFunctions.o String.o AbstractReflectSession.o ReflectServer.o SocketMultiplexer.o NetworkUtilityFunctions.o SysLog.o PulseNode.o SetupSystem.o ServerComponent.o ByteBuffer.o FileDescriptorDataIO.o StringMatcher.o SystemInfo.o executable_diff.o

# Where to find .cpp files
VPATH = muscle/message muscle/dataio muscle/regex muscle/iogateway muscle/reflector muscle/util muscle/syslog muscle/system

# if the OS type variable is unset, try to set it using the uname shell command
ifeq ($(OSTYPE),)
  OSTYPE = $(shell uname)
endif

# IRIX may report itself as IRIX or as IRIX64.  They are both the same to us.
ifeq ($(OSTYPE),IRIX64)
  OSTYPE = IRIX
endif

ifeq ($(OSTYPE),beos)
  ifeq ($(BE_HOST_CPU),ppc)
    CXX = mwcc
  else # not ppc
    CXXFLAGS += $(GCCFLAGS) $(CCOPTFLAGS)
    LIBS = -lbe -lnet -lroot
    ifeq ($(shell ls 2>/dev/null -1 /boot/develop/headers/be/bone/bone_api.h), /boot/develop/headers/be/bone/bone_api.h)
      CXXFLAGS += -I/boot/develop/headers/be/bone -DBONE
      LIBS = -nodefaultlibs -lbind -lsocket -lbe -lroot -L/boot/beos/system/lib
    endif
  endif # END ifeq ($(BE_HOST_CPU),ppc)
else # not beos
  CXXFLAGS += $(GCCFLAGS) $(CCOPTFLAGS)
  ifeq ($(OSTYPE),freebsd4.0)
    CXXFLAGS += -I/usr/include/machine
  else # not freebsd4.0
    ifeq ($(OSTYPE),Darwin)
      CXXFLAGS += -std=c++11
      LFLAGS += -framework Foundation -framework Carbon -framework SystemConfiguration
    else # not darwin
      ifeq ($(OSTYPE),IRIX)
        CXXFLAGS += -DSGI -DMIPS
        ifneq (,$(findstring g++,$(CXX))) # if we are using SGI's CC compiler, we gotta change a few things
          CXX = CC
          CCFLAGS = -g2 -n32 -LANG:std -woff 1110,1174,1552,1460,3303
          LFLAGS  = -g2 -n32
          CXXFLAGS += -DNEW_H_NOT_AVAILABLE
        endif # END ifneq (,$(findstring g++,$(CXX)))
      endif # END ifeq ($(OSTYPE),IRIX)
    endif # END ifeq ($(OSTYPE),darwin)
  endif # END ifeq ($(OSTYPE),freebsd4.0)
endif #END ifeq ($(OSTYPE),beos)

all : $(EXECUTABLES)

optimized : CCOPTFLAGS = -O3
optimized : all

executable_diff : $(OBJFILES) $(ZLIBOBJS)
	$(CXX) $(LIBS) $(LFLAGS) -o $@ $^

clean :
	rm -f *.o *.xSYM $(EXECUTABLES)
