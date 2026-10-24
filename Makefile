## you have command.cc command.h tokenizer.cc tokenizer.h you need compile them and link
## them to one object file called  : "myshell" 
## so we should run make , that will create our object myshell ,
## ./myshell should run your application

CXX      = g++
CXXFLAGS = -Wall -Wextra -g -std=c++17
OBJS     = command.o tokenizer.o

myshell: $(OBJS)
	$(CXX) $(CXXFLAGS) -o myshell $(OBJS)

%.o: %.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c $<

quiet: CXXFLAGS += -DNO_TABLE
quiet: clean myshell

clean:
	rm -f *.o myshell
