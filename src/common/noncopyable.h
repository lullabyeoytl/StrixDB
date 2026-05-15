#ifndef NONCOPYABLE_H
#define NONCOPYABLE_H
#pragma once

class NonCopyable {
public:
    NonCopyable() = default;
    ~NonCopyable() = default;
protected:  
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

#endif  // NONCOPYABLE_H