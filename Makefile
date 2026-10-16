UNAME_S = $(shell uname -s)

OBJ = main.o pseudo_bios.o loadelf.o disassemble.o helper.o interpret.o sparse_mem.o cache_model.o sgi_mc.o sgi_hpc.o sgi_scc.o sgi_scsi.o sgi_seeq.o saveState.o gdbstub.o githash.o shmem.o cosim.o sgi_newport.o newport_sdl.o sgi_ps2.o

ifeq ($(UNAME_S),Linux)
	CXX = clang++-20 -march=native -flto
	EXTRA_LD = -ldl -lffi -lbfd -lunwind -lboost_program_options -lboost_serialization -lcapstone
endif

ifeq ($(UNAME_S),FreeBSD)
	CXX = CC -march=native
	EXTRA_LD = -L/usr/local/lib -lunwind -lboost_program_options -lcapstone
endif

ifeq ($(UNAME_S),Darwin)
	CXX = clang++ -march=native -I/opt/local/include
	EXTRA_LD = -L/opt/local/lib -lboost_program_options-mt -lcapstone
endif

CXXFLAGS = -std=c++17 -g $(OPT)
LIBS =  $(EXTRA_LD) -lpthread

# SDL2 (optional): the live Newport display (--sdl).  Without it the build is
# headless and NEWPORT_PPM=<file> is the way to see the screen.
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)
ifneq ($(SDL_LIBS),)
	CXXFLAGS += -DHAVE_SDL $(shell sdl2-config --cflags)
	LIBS += $(SDL_LIBS)
endif

DEP = $(OBJ:.o=.d)
OPT = -O3 -g -fomit-frame-pointer -std=c++17
EXE = interp_mips

# Berkeley SoftFloat-3 (git submodule ./softfloat) -- bit-exact IEEE-754 with an
# explicit rounding mode.  The interp calls it via extern "C".  Built by the
# submodule's own Makefile (8086-SSE build: FAST_INT64 object set); its NaN
# payloads differ from MIPS but that doesn't affect finite-value rounding.
SF_BUILD = softfloat/build/Linux-x86_64-GCC
SF_LIB   = $(SF_BUILD)/softfloat.a

.PHONY : all clean

# Newport tools: whole-trace replay (NEWPORT_TRACE) and the per-GO oracle (NEWPORT_DRAWCAP)
NP_TOOL_OBJ = $(filter-out main.o,$(OBJ))
tools/np_replay: tools/np_replay.cc $(NP_TOOL_OBJ) $(SF_LIB)
	$(CXX) $(CXXFLAGS) $< $(NP_TOOL_OBJ) $(SF_LIB) $(LIBS) -o $@
tools/np_goreplay: tools/np_goreplay.cc $(NP_TOOL_OBJ) $(SF_LIB)
	$(CXX) $(CXXFLAGS) $< $(NP_TOOL_OBJ) $(SF_LIB) $(LIBS) -o $@

all: $(EXE)

$(EXE) : $(OBJ) $(SF_LIB)
	$(CXX) $(CXXFLAGS) $(OBJ) $(SF_LIB) $(LIBS) -o $(EXE)

$(SF_LIB):
	$(MAKE) -C $(SF_BUILD)

githash.cc : .git/HEAD .git/index
	echo "const char *githash = \"$(shell git rev-parse HEAD)\";" > $@

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 


-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
	-$(MAKE) -C $(SF_BUILD) clean
