#pragma once

class GraphScene;

class GraphMerger {
public:
    struct Stats {
        int kept = 0;
        int added = 0;
        int stale = 0;
    };

    static Stats merge(GraphScene* curated, const GraphScene* generated);
};
