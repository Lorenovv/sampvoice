#include <sdk.hpp>

class SampVoiceOMPProbe final : public IComponent
{
public:
    StringView componentName() const override
    {
        return "SampVoiceOMPProbe";
    }

    SemanticVersion componentVersion() const override
    {
        return SemanticVersion(0, 0, 1);
    }

    UID getUID() override
    {
        return UID(0x0f7cdb94c1a68531);
    }

    void onLoad(ICore*) override { }
    void free() override { delete this; }
    void reset() override { }
};

COMPONENT_ENTRY_POINT()
{
    return new SampVoiceOMPProbe();
}
