FROM gcc:13
WORKDIR /app
COPY backend ./backend
COPY frontend ./frontend
WORKDIR /app/backend
RUN curl -L -o httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h \
 && curl -L -o json.hpp https://github.com/nlohmann/json/releases/latest/download/json.hpp \
 && g++ -std=c++17 -O2 main.cpp -o campusconnect -pthread
EXPOSE 8080
CMD ["./campusconnect"]