{
  programs = {
    nixfmt.enable = true;
    clang-format.enable = true;
    prettier = {
      enable = true;
      includes = [
        "*.md"
        "*.yml"
        "*.yaml"
      ];
      settings.proseWrap = "always";
    };
    taplo.enable = true;
  };
}
