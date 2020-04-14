import { AfterViewInit, Directive, ElementRef, Input, Optional } from '@angular/core';
import { Permissions } from '../models/permissions';
import { AuthStorageService } from '../services/auth-storage.service';
import { PermissionScopeDirective } from './permission-scope.directive';

@Directive({
  selector: 'input:not([cdNoUpdateScope]), select:not([cdNoUpdateScope]), [cdUpdateScope]'
})
export class UpdateScopeDirective implements AfterViewInit {
  private permissions: Permissions;
  private service_name: keyof Permissions;

  @Input() disabled: boolean;

  constructor(
    @Optional() private scope: PermissionScopeDirective,
    private authStorageService: AuthStorageService,
    private elementRef: ElementRef
  ) {
    this.permissions = this.authStorageService.getPermissions();
  }

  ngAfterViewInit() {
    if (this.scope !== null) {
      this.service_name = this.scope.cdScope;
    }
    if (
      this.service_name !== undefined &&
      (this.disabled || !this.permissions[this.service_name].update)
    ) {
      this.elementRef.nativeElement.disabled = true;
    }
  }
}
