class Counter : gameObject
{
    float elapsed = 0;
    int frames = 0;
    string label = "Counter";

    func start()
    {
        elapsed = 0;
        frames = 0;
    }

    func update(float delta)
    {
        elapsed = elapsed + delta;
        frames = frames + 1;
    }

    func draw()
    {
    }

    func seconds() : float
    {
        return elapsed;
    }
}

class FastCounter : Counter
{
    func update(float delta)
    {
        elapsed = elapsed + delta * 2;
        frames = frames + 1;
    }
}
