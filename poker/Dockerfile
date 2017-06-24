FROM ubuntu
RUN apt-get install build-essential cmake libboost-all-dev

# Copy the local package files to the container's workspace.
ADD . /Users/nidhikulkarni/Desktop/PokerApp/poker 

RUN cd poker/src
RUN mkdir build && cd build
RUN cmake .
RUN make

ENTRYPOINT ./holdingnuts-server