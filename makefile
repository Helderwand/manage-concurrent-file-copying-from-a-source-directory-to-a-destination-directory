all : clean compile 

compile : 
	@gcc -g -o MWCp hw4_main.c -lpthread


clean :
	@rm -f *.o
	@rm -f MWCp
	
	
