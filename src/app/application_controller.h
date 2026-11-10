#pragma once

#include <QObject>

#include <memory>

class QApplication;

namespace phoschat {

class ApplicationController final : public QObject {
public:
    explicit ApplicationController(QApplication &application);
    ~ApplicationController() override;
    bool start();

private:
    struct Components;

    QApplication &application_;
    std::unique_ptr<Components> components_;
};

} // namespace phoschat
